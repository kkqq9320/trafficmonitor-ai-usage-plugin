# Build Guide

## Requirements

- Windows
- Visual Studio 2022 or Build Tools 2022
- Desktop development with C++
- MSVC `v143` toolset
- MFC for the `v143` toolset (`UseOfMfc=Dynamic`)
- Windows SDK selected by Visual Studio

## Build

Open `ClaudeUsagePlugin.sln` in Visual Studio and build `Release|x64` or `Release|Win32`, or run:

```powershell
MSBuild.exe .\ClaudeUsagePlugin.sln /t:ClaudeUsagePlugin /p:Configuration=Release /p:Platform=x64
```

For `Win32`:

```powershell
MSBuild.exe .\ClaudeUsagePlugin.sln /t:ClaudeUsagePlugin /p:Configuration=Release /p:Platform=Win32
```

Run the helper watch-lock regression test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-claude-web-helper-watch-lock.ps1
```

The test confirms that a stale lock cannot cause an unrelated reused PID to be
treated as, or terminated as, the helper watcher.

Run the Codex selection regression tests (builds first, then loads the DLL for each scenario):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-codex-window-classification.ps1 -Platform x64
```

Run the helper tests (Node.js 22+) and the Codex helper wrapper test:

```powershell
node --test helper/codex-usage-helper/test/lib.test.mjs helper/codex-usage-helper/test/io.test.mjs helper/claude-web-helper/test/retry.test.mjs helper/claude-web-helper/test/activity.test.mjs helper/claude-web-helper/test/reset-credits.test.mjs helper/claude-web-helper/test/usage-payload.test.mjs
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-codex-usage-helper-wrapper.ps1
```

These tests use temporary `LOCALAPPDATA` / `CODEX_HOME` folders and fake HTTP responses; they do not start Codex or contact OpenAI or Anthropic.

To print what a built DLL shows for the current user profile:

```powershell
.\build\x64\Release\tests\CodexUsagePluginTests.exe .\build\x64\Release\plugins\ClaudeUsagePlugin.dll --print
```

## Build Output

- `build\x64\Release\plugins\ClaudeUsagePlugin.dll`
- `build\x64\Release\plugins\ClaudeUsagePlugin\claude-web-helper.ps1`
- `build\x64\Release\plugins\ClaudeUsagePlugin\codex-usage-helper.ps1`
- `build\x64\Release\plugins\ClaudeUsagePlugin\helper-common.ps1`
- `build\x64\Release\plugins\ClaudeUsagePlugin\helper\claude-web-helper\...`
- `build\x64\Release\plugins\ClaudeUsagePlugin\helper\codex-usage-helper\...`
- `build\Release\plugins\ClaudeUsagePlugin.dll`
- `build\Release\plugins\ClaudeUsagePlugin\claude-web-helper.ps1`
- `build\Release\plugins\ClaudeUsagePlugin\helper\claude-web-helper\...`

Release assets are x64 only. The project also builds `Win32` (used by the tests) and `ARM64EC`.

## Try a Local Build

To run a build before releasing it, replace an installed copy the same way as [Update to a new version](install.md#update-to-a-new-version): exit TrafficMonitor, stop both helpers, then copy `build\x64\Release\plugins\ClaudeUsagePlugin.dll` and the `ClaudeUsagePlugin` folder next to it into the TrafficMonitor `plugins` folder.

## Packaging

`scripts/package-release.ps1` builds the release zip:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1 -Version <version>
```

- Stops when `<version>` differs from `TMI_VERSION` in `src/ClaudeUsagePlugin/ClaudeUsagePlugin.cpp`
- Rebuilds `Release|x64` (`-SkipBuild` packages the existing output)
- Writes `dist\TrafficMonitorAIUsageLimits_v<version>_x64.zip` with only `plugins\...`: the DLL, both helpers, and `LICENSE`, `NOTICE.md`, `PRIVACY.md` under `plugins\ClaudeUsagePlugin`
- Checks that every helper file is in the zip and prints its SHA-256

Publishing follows [release-checklist.md](release-checklist.md).
