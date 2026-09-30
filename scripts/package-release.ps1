param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [switch]$SkipBuild
)

# Builds Release|x64 and writes dist\TrafficMonitorAIUsageLimits_v<Version>_x64.zip.
# The zip holds only plugins\...; extracting it into the TrafficMonitor folder installs or updates the plugin.

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path $PSScriptRoot -Parent
$pluginSource = Join-Path $repoRoot 'src\ClaudeUsagePlugin\ClaudeUsagePlugin.cpp'
$outputRoot = Join-Path $repoRoot 'build\x64\Release\plugins'
$distRoot = Join-Path $repoRoot 'dist'
$zipPath = Join-Path $distRoot "TrafficMonitorAIUsageLimits_v${Version}_x64.zip"

# The version TrafficMonitor shows must match the tag and the asset name.
$source = Get-Content -LiteralPath $pluginSource -Raw
if ($source -notmatch 'case TMI_VERSION:\s*value = L"([^"]+)"') {
    throw "TMI_VERSION was not found in $pluginSource."
}
if ($Matches[1] -ne $Version) {
    throw "TMI_VERSION is '$($Matches[1])', not '$Version'. Update ClaudeUsagePlugin.cpp first."
}

if (-not $SkipBuild) {
    $msbuildCommand = Get-Command MSBuild.exe -CommandType Application -ErrorAction SilentlyContinue
    $msbuildPath = if ($msbuildCommand) { $msbuildCommand.Source } else { $null }
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not $msbuildPath -and (Test-Path $vswherePath)) {
        $msbuildPath = & $vswherePath -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
            Select-Object -First 1
    }
    if (-not $msbuildPath) {
        throw 'MSBuild.exe was not found. Install Visual Studio 2022 or Build Tools 2022 (see docs/build.md).'
    }
    & $msbuildPath (Join-Path $repoRoot 'ClaudeUsagePlugin.sln') /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }
}

# Files every release must carry; a missing helper file breaks that helper at runtime.
$payload = @(
    'plugins/ClaudeUsagePlugin.dll'
    'plugins/ClaudeUsagePlugin/claude-web-helper.ps1'
    'plugins/ClaudeUsagePlugin/codex-usage-helper.ps1'
    'plugins/ClaudeUsagePlugin/helper-common.ps1'
    'plugins/ClaudeUsagePlugin/helper/claude-web-helper/index.mjs'
    'plugins/ClaudeUsagePlugin/helper/claude-web-helper/package.json'
    'plugins/ClaudeUsagePlugin/helper/claude-web-helper/package-lock.json'
    'plugins/ClaudeUsagePlugin/helper/codex-usage-helper/index.mjs'
    'plugins/ClaudeUsagePlugin/helper/codex-usage-helper/lib.mjs'
    'plugins/ClaudeUsagePlugin/helper/codex-usage-helper/package.json'
)
$sources = @{}
foreach ($entry in $payload) {
    $sources[$entry] = Join-Path $outputRoot ($entry.Substring('plugins/'.Length) -replace '/', '\')
}
# License, notices and privacy notes travel next to the helpers instead of the TrafficMonitor root.
foreach ($name in 'LICENSE', 'NOTICE.md', 'PRIVACY.md') {
    $sources["plugins/ClaudeUsagePlugin/$name"] = Join-Path $repoRoot $name
}

$missing = @($sources.GetEnumerator() | Where-Object { -not (Test-Path -LiteralPath $_.Value -PathType Leaf) } | ForEach-Object { $_.Value })
if ($missing.Count -gt 0) {
    throw "Missing release files:`n$($missing -join "`n")"
}

New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

# ZipArchive with '/' separators, so every unzip tool sees the same folders.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in ($sources.Keys | Sort-Object)) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $sources[$entry], $entry, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
}
finally {
    $zip.Dispose()
}

$check = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $entries = @($check.Entries | ForEach-Object { $_.FullName })
}
finally {
    $check.Dispose()
}
$absent = @($sources.Keys | Where-Object { $entries -notcontains $_ })
if ($absent.Count -gt 0) {
    throw "The zip is missing:`n$($absent -join "`n")"
}

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
Write-Host "Created $zipPath"
Write-Host "Entries: $($entries.Count)"
Write-Host "SHA256: $hash"
