param(
    [ValidateSet('once', 'watch', 'start', 'status', 'stop')]
    [string]$Mode = 'status'
)

# Codex usage helper wrapper for the TrafficMonitor AI Usage Limits plugin.
#   once   one server request (codex app-server, then wham/usage), falling back to session JSONL
#   start  run "watch" as a hidden background process (session JSONL push + scheduled server refresh)
#   watch  run the watcher in the foreground
#   status show snapshot, helper status, watch lock and Node.js path
#   stop   stop the validated background watcher
# Optional settings in %LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\helper-config.json:
#   node_path, codex_path, codex_server_refresh_minutes (default 15, minimum 0.5)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'helper-common.ps1')

$helperMarker = 'codex-usage-helper'
$helperDirCandidates = @(
    (Join-Path $PSScriptRoot 'helper\codex-usage-helper'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'helper\codex-usage-helper')
)
$helperDir = $helperDirCandidates |
    Where-Object { Test-Path (Join-Path $_ 'index.mjs') } |
    Select-Object -First 1
if (-not $helperDir) {
    throw ("Codex usage helper index.mjs not found. Checked: {0}" -f ($helperDirCandidates -join ', '))
}
$helperDir = (Resolve-Path $helperDir).Path
$helperScriptPath = Join-Path $helperDir 'index.mjs'

$baseDir = Get-AiUsageHelperBaseDir
$snapshotPath = Join-Path $baseDir 'codex-usage.json'
$statusPath = Join-Path $baseDir 'codex-usage-helper-status.json'
$watchLockPath = Join-Path $baseDir 'codex-usage-helper-watch.lock'

function Get-ValidatedWatch {
    $watchLock = Get-AiUsageHelperWatchLock -Path $watchLockPath
    if (-not $watchLock) {
        return $null
    }
    $watchPid = 0
    [void][int]::TryParse([string]$watchLock.pid, [ref]$watchPid)
    if (Test-AiUsageHelperWatchProcess -ProcessId $watchPid -WatchLock $watchLock -Marker $helperMarker) {
        return [pscustomobject]@{ Pid = $watchPid; Lock = $watchLock }
    }
    return $null
}

function Remove-StaleWatchLock {
    if ((Test-Path -LiteralPath $watchLockPath) -and -not (Get-ValidatedWatch)) {
        Remove-Item -LiteralPath $watchLockPath -Force -ErrorAction SilentlyContinue
    }
}

function Resolve-NodeOrWriteStatus {
    try {
        return Resolve-AiUsageHelperNode -MinimumMajor 22
    } catch {
        Write-AiUsageHelperStatus -Path $statusPath -State 'node_missing' -ErrorText $_.Exception.Message
        throw
    }
}

function Format-UsageWindow {
    param([object]$Window)

    if (-not $Window) {
        return '--'
    }
    $reset = '?'
    if ($Window.resets_at) {
        $reset = [DateTimeOffset]::FromUnixTimeSeconds([int64]$Window.resets_at).ToLocalTime().ToString('yyyy-MM-dd HH:mm')
    }
    return ('{0}% (resets {1})' -f $Window.used_percent, $reset)
}

function Show-Status {
    Write-Host "Helper dir: $helperDir"
    try {
        $nodePath = Resolve-AiUsageHelperNode -MinimumMajor 22
        Write-Host "Node: $nodePath ($(& $nodePath --version))"
    } catch {
        Write-Host "Node: unavailable ($($_.Exception.Message))"
    }
    Write-Host "Config: $(Get-AiUsageHelperConfigPath)"

    $watch = Get-ValidatedWatch
    if ($watch) {
        Write-Host ("Watch: pid={0}, started_at={1}, refresh_ms={2}" -f $watch.Pid, $watch.Lock.started_at, $watch.Lock.refresh_ms)
    } elseif (Test-Path -LiteralPath $watchLockPath) {
        Write-Host 'Watch: stale lock'
    } else {
        Write-Host 'Watch: not running'
    }

    if (Test-Path -LiteralPath $snapshotPath) {
        try {
            $snapshot = Get-Content -LiteralPath $snapshotPath -Raw | ConvertFrom-Json
            Write-Host ("Snapshot: source={0}/{1}, data_at={2}, plan={3}" -f $snapshot.source, $snapshot.method, $snapshot.data_at, $snapshot.plan_type)
            Write-Host ("  5h: {0}" -f (Format-UsageWindow $snapshot.five_hour))
            Write-Host ("  7d: {0}" -f (Format-UsageWindow $snapshot.seven_day))
            Write-Host ("  limit_reached={0} ({1})" -f $snapshot.limit_reached, $snapshot.rate_limit_reached_type)
        } catch {
            Write-Host "Snapshot: unreadable ($($_.Exception.Message))"
        }
    } else {
        Write-Host 'Snapshot: none'
    }

    if (Test-Path -LiteralPath $statusPath) {
        try {
            $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
            Write-Host ("Status: {0} (updated {1})" -f $status.state, $status.updated_at)
            if ($status.server) {
                Write-Host ("  server: last_success={0}, next={1}, failures={2}, retry_after={3}, method={4}" -f $status.server.last_success_at, $status.server.next_fetch_at, $status.server.consecutive_failures, $status.server.retry_after_until, $status.server.last_method)
            }
            if ($status.codex_path) {
                Write-Host ("  codex: {0} ({1})" -f $status.codex_path, $status.codex_origin)
            }
            if ($status.error) {
                Write-Host "  error: $($status.error)"
            }
        } catch {
            Write-Host "Status: unreadable ($($_.Exception.Message))"
        }
    }
}

function Stop-HelperWatch {
    $watch = Get-ValidatedWatch
    if ($watch) {
        Stop-Process -Id $watch.Pid -Force
        Start-Sleep -Milliseconds 500
        Remove-Item -LiteralPath $watchLockPath -Force -ErrorAction SilentlyContinue
        Write-Host "Stopped Codex usage helper watch PID $($watch.Pid)"
        return
    }
    if (Test-Path -LiteralPath $watchLockPath) {
        Remove-Item -LiteralPath $watchLockPath -Force -ErrorAction SilentlyContinue
        Write-Host 'Removed stale Codex usage helper watch lock.'
        return
    }
    Write-Host 'Codex usage helper watch is not running.'
}

function Start-HiddenWatch {
    Remove-StaleWatchLock
    $watch = Get-ValidatedWatch
    if ($watch) {
        Write-Host "Codex usage helper watch already running (PID $($watch.Pid))."
        return
    }

    $nodePath = Resolve-NodeOrWriteStatus
    $escapedScriptPath = $helperScriptPath.Replace('"', '\"')
    $child = Start-Process -FilePath $nodePath -WorkingDirectory $helperDir -ArgumentList "`"$escapedScriptPath`" watch" -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 1
    Write-Host "Started Codex usage helper watch in background (PID $($child.Id))."
}

switch ($Mode) {
    'start' {
        Start-HiddenWatch
        exit 0
    }
    'status' {
        Remove-StaleWatchLock
        Show-Status
        exit 0
    }
    'stop' {
        Stop-HelperWatch
        exit 0
    }
    default {
        $nodePath = Resolve-NodeOrWriteStatus
        & $nodePath $helperScriptPath $Mode
        exit $LASTEXITCODE
    }
}
