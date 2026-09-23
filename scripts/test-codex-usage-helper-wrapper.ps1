param()

# Exercises scripts\codex-usage-helper.ps1 in a temporary LOCALAPPDATA / CODEX_HOME.
# No Codex executable is used (CODEX_USAGE_HELPER_CODEX_PATH points to a missing file) and there is
# no auth.json, so no network request leaves this machine.

$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Write-SessionEvent {
    param([string]$Path, [int]$UsedPercent)
    $timestamp = [DateTimeOffset]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    $line = '{"timestamp":"' + $timestamp + '","type":"event_msg","payload":{"type":"token_count","info":null,"rate_limits":{"limit_id":"codex","primary":{"used_percent":' + $UsedPercent + ',"window_minutes":10080,"resets_at":1893456000},"secondary":null}}}' + "`n"
    [System.IO.File]::AppendAllText($Path, $line, [System.Text.UTF8Encoding]::new($false))
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$wrapperPath = Join-Path $repoRoot 'scripts\codex-usage-helper.ps1'
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('trafficmonitor-codex-helper-test-' + [guid]::NewGuid().ToString('N'))
$baseDir = Join-Path $testRoot 'local\trafficmonitor-claude-usage-plugin'
$codexHome = Join-Path $testRoot 'codex'
$sessionDir = Join-Path $codexHome 'sessions\2026\09\23'
$sessionPath = Join-Path $sessionDir 'rollout-test.jsonl'
$snapshotPath = Join-Path $baseDir 'codex-usage.json'
$watchLockPath = Join-Path $baseDir 'codex-usage-helper-watch.lock'
$saved = @{
    LOCALAPPDATA = $env:LOCALAPPDATA
    CODEX_HOME = $env:CODEX_HOME
    CODEX_USAGE_HELPER_CODEX_PATH = $env:CODEX_USAGE_HELPER_CODEX_PATH
}
$unrelatedProcess = $null

try {
    New-Item -ItemType Directory -Force -Path $baseDir, $sessionDir | Out-Null
    . (Join-Path $repoRoot 'scripts\helper-common.ps1')
    $env:LOCALAPPDATA = Join-Path $testRoot 'local'
    $nodePath = Resolve-AiUsageHelperNode -MinimumMajor 22
    $env:CODEX_HOME = $codexHome
    $env:CODEX_USAGE_HELPER_CODEX_PATH = Join-Path $testRoot 'missing\codex.exe'

    Write-SessionEvent -Path $sessionPath -UsedPercent 42
    & $wrapperPath once | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'once did not produce a snapshot.'
    $snapshot = Get-Content $snapshotPath -Raw | ConvertFrom-Json
    Assert-True ($snapshot.source -eq 'jsonl' -and $snapshot.seven_day.used_percent -eq 42) 'once did not fall back to the session JSONL value.'
    Write-Host 'PASS once falls back to session JSONL when no server source is available.'

    $dummyScript = Join-Path $testRoot 'index.mjs'
    [System.IO.File]::WriteAllText($dummyScript, 'setInterval(() => {}, 60000);', [System.Text.UTF8Encoding]::new($false))
    $unrelatedProcess = Start-Process -FilePath $nodePath -ArgumentList $dummyScript, 'watch' -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500
    $unrelatedProcess.Refresh()
    $lock = [ordered]@{ pid = $unrelatedProcess.Id; mode = 'watch'; started_at = ([DateTimeOffset]$unrelatedProcess.StartTime).ToUniversalTime().ToString('o'); refresh_ms = 900000 }
    [System.IO.File]::WriteAllText($watchLockPath, ($lock | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))

    & $wrapperPath start | Out-Null
    $watchPid = 0
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        Start-Sleep -Milliseconds 500
        $current = Get-Content $watchLockPath -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json
        if ($current -and [int]$current.pid -ne $unrelatedProcess.Id) {
            $watchPid = [int]$current.pid
            break
        }
    }
    Assert-True ($watchPid -gt 0) 'start did not replace a lock that points at an unrelated node process.'
    $unrelatedProcess.Refresh()
    Assert-True (-not $unrelatedProcess.HasExited) 'start terminated an unrelated process.'
    $commandLine = (Get-CimInstance Win32_Process -Filter "ProcessId = $watchPid").CommandLine
    Assert-True ($commandLine -match 'codex-usage-helper' -and $commandLine -match 'watch') "Unexpected watcher command line: $commandLine"
    Write-Host 'PASS start launches the watcher with the pinned Node.js and ignores an unrelated lock owner.'

    Start-Sleep -Seconds 1
    Write-SessionEvent -Path $sessionPath -UsedPercent 97
    $updated = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        Start-Sleep -Milliseconds 500
        $snapshot = Get-Content $snapshotPath -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json
        if ($snapshot -and $snapshot.seven_day.used_percent -eq 97) {
            $updated = $true
            break
        }
    }
    Assert-True $updated 'The running watcher did not apply a new session event.'
    Assert-True ($snapshot.method -eq 'session-event' -and $snapshot.limit_reached -eq $false) 'Unexpected snapshot after the session event.'
    Write-Host 'PASS the watcher pushes a new session event into the snapshot.'

    & $wrapperPath status | Out-Null
    Assert-True (Test-Path $watchLockPath) 'status removed the lock of the running watcher.'
    & $wrapperPath stop | Out-Null
    Start-Sleep -Milliseconds 500
    Assert-True (-not (Get-Process -Id $watchPid -ErrorAction SilentlyContinue)) 'stop did not stop the watcher.'
    Assert-True (-not (Test-Path $watchLockPath)) 'stop did not remove the watch lock.'
    $unrelatedProcess.Refresh()
    Assert-True (-not $unrelatedProcess.HasExited) 'stop terminated an unrelated process.'
    Write-Host 'PASS status keeps and stop removes only the validated watcher.'
}
finally {
    if ($unrelatedProcess) {
        $unrelatedProcess.Refresh()
        if (-not $unrelatedProcess.HasExited) {
            Stop-Process -Id $unrelatedProcess.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Get-CimInstance Win32_Process -Filter "Name = 'node.exe'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains('codex-usage-helper') -and $_.CommandLine.Contains($repoRoot) -and $_.CommandLine -match '\bwatch\b' } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
    foreach ($name in $saved.Keys) {
        Set-Item -Path "Env:$name" -Value $saved[$name] -ErrorAction SilentlyContinue
        if ($null -eq $saved[$name]) {
            Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
        }
    }
    $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    if ($resolvedTestRoot.StartsWith($resolvedTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolvedTestRoot)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host 'All Codex usage helper wrapper tests passed.'
