# Shared functions for the TrafficMonitor AI usage helper wrappers.
# Dot-source this file: . (Join-Path $PSScriptRoot 'helper-common.ps1')

function Get-AiUsageHelperBaseDir {
    if ($env:LOCALAPPDATA) {
        return Join-Path $env:LOCALAPPDATA 'trafficmonitor-claude-usage-plugin'
    }
    return Join-Path $env:USERPROFILE '.cache\trafficmonitor-claude-usage-plugin'
}

function Get-AiUsageHelperConfigPath {
    return Join-Path (Get-AiUsageHelperBaseDir) 'helper-config.json'
}

function Read-AiUsageHelperConfig {
    $path = Get-AiUsageHelperConfigPath
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        try {
            $config = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            if ($config) {
                return $config
            }
        } catch {
            Write-Warning "Ignoring unreadable helper config: $path"
        }
    }
    return [pscustomobject]@{}
}

function Save-AiUsageHelperConfig {
    param([object]$Config)

    $path = Get-AiUsageHelperConfigPath
    New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
    $json = $Config | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText($path, $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
}

function Write-AiUsageHelperStatus {
    param(
        [string]$Path,
        [string]$State,
        [string]$ErrorText
    )

    New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
    $payload = [ordered]@{
        state = $State
        updated_at = [DateTimeOffset]::UtcNow.ToString('o')
        error = $ErrorText
    }
    [System.IO.File]::WriteAllText($Path, ($payload | ConvertTo-Json) + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
}

function Get-NodeMajorVersion {
    param([string]$NodePath)

    try {
        $version = & $NodePath -p 'process.versions.node' 2>$null
        if ($LASTEXITCODE -ne 0) {
            return 0
        }
        return [int](([string]$version).Trim().Split('.')[0])
    } catch {
        return 0
    }
}

function Get-StandardNodeCandidates {
    $candidates = @()
    if ($env:ProgramFiles) {
        $candidates += Join-Path $env:ProgramFiles 'nodejs\node.exe'
    }
    if (${env:ProgramFiles(x86)}) {
        $candidates += Join-Path ${env:ProgramFiles(x86)} 'nodejs\node.exe'
    }
    if ($env:LOCALAPPDATA) {
        $candidates += Join-Path $env:LOCALAPPDATA 'Programs\nodejs\node.exe'
    }
    return $candidates
}

function Test-AiUsageHelperNode {
    param(
        [string]$NodePath,
        [int]$MinimumMajor
    )

    return $NodePath -and (Test-Path -LiteralPath $NodePath -PathType Leaf) -and ((Get-NodeMajorVersion $NodePath) -ge $MinimumMajor)
}

# Resolves the Node.js executable for the helpers without consulting PATH, so another app's
# bundled node.exe that happens to be first on PATH is never used.
# Order: TRAFFICMONITOR_AI_USAGE_NODE, node_path pinned in helper-config.json, then the standard
# Node.js install locations. The first standard location that works is pinned into helper-config.json.
function Resolve-AiUsageHelperNode {
    param([int]$MinimumMajor = 22)

    $override = [string]$env:TRAFFICMONITOR_AI_USAGE_NODE
    if ($override) {
        if (Test-AiUsageHelperNode -NodePath $override -MinimumMajor $MinimumMajor) {
            return $override
        }
        throw "TRAFFICMONITOR_AI_USAGE_NODE is not Node.js $MinimumMajor or newer: $override"
    }

    $config = Read-AiUsageHelperConfig
    $pinned = [string]$config.node_path
    if ($pinned) {
        if (Test-AiUsageHelperNode -NodePath $pinned -MinimumMajor $MinimumMajor) {
            return $pinned
        }
        throw "node_path in $(Get-AiUsageHelperConfigPath) is not Node.js $MinimumMajor or newer: $pinned"
    }

    foreach ($candidate in Get-StandardNodeCandidates) {
        if (Test-AiUsageHelperNode -NodePath $candidate -MinimumMajor $MinimumMajor) {
            $config | Add-Member -NotePropertyName node_path -NotePropertyValue $candidate -Force
            Save-AiUsageHelperConfig $config
            return $candidate
        }
    }

    throw ("Node.js $MinimumMajor or newer was not found in the standard install locations. " +
        "Install Node.js LTS (winget install OpenJS.NodeJS.LTS) or set node_path in $(Get-AiUsageHelperConfigPath).")
}

function Get-AiUsageHelperWatchLock {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return $null
    }
    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    } catch {
        return $null
    }
}

# True only when the PID in the lock is a node.exe running <Marker>\index.mjs watch that started
# within 30 seconds of the lock's started_at (protects against PID reuse).
function Test-AiUsageHelperWatchProcess {
    param(
        [int]$ProcessId,
        [object]$WatchLock,
        [string]$Marker
    )

    if ($ProcessId -le 0 -or -not $WatchLock -or [string]$WatchLock.mode -ne 'watch') {
        return $false
    }

    try {
        $process = Get-CimInstance Win32_Process -Filter "ProcessId = $ProcessId" -ErrorAction Stop
        if (-not $process -or $process.Name -ne 'node.exe') {
            return $false
        }

        $commandLine = [string]$process.CommandLine
        if ($commandLine -notmatch '(?i)index\.mjs' -or $commandLine -notmatch '(?i)(^|\s)watch(\s|$)') {
            return $false
        }
        if ($Marker -and $commandLine.IndexOf($Marker, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
            return $false
        }

        $lockStartedAt = [DateTimeOffset]::Parse(
            [string]$WatchLock.started_at,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::AssumeUniversal
        ).ToUniversalTime()
        $processStartedAt = ([DateTimeOffset]$process.CreationDate).ToUniversalTime()
        return [Math]::Abs(($lockStartedAt - $processStartedAt).TotalSeconds) -le 30
    } catch {
        return $false
    }
}
