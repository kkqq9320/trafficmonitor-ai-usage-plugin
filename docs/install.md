# Install Guide

Languages: English | [한국어](install.ko.md)

This guide installs the plugin from a release zip. To build from source instead, see [build.md](build.md).

[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) is a Windows system monitor that can show network speed, CPU and memory in a floating window or in the taskbar. The release zip carries only the plugin, so TrafficMonitor is installed separately.

## Requirements

- Windows x64 and the x64 build of TrafficMonitor. The plugin is not released for x86.
- Node.js 22 or newer in a standard install location (`%ProgramFiles%\nodejs` and similar). Both helpers ignore PATH; another location goes into `node_path` in [helper settings](runtime.md#helper-settings).
- Microsoft Edge or Google Chrome for the one-time Claude sign-in.
- For Codex values, Codex (desktop app or CLI) signed in on the same PC.

## First install

### 1. Install TrafficMonitor

1. Download the x64 package from [TrafficMonitor Releases](https://github.com/zhongyang219/TrafficMonitor/releases). The Lite package is enough unless you want temperature monitoring.
2. Extract it to any folder, for example `D:\TrafficMonitor`.
3. Run `TrafficMonitor.exe` once to confirm it starts, then exit it (right-click the tray icon, `Exit`).

### 2. Install Node.js

Skip this if `node --version` already prints 22 or newer.

```powershell
winget install OpenJS.NodeJS.LTS
```

### 3. Extract the plugin

1. Download `TrafficMonitorAIUsageLimits_v<version>_x64.zip` from the [latest release](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin/releases/latest).
2. Make sure TrafficMonitor is not running.
3. Extract the zip into the folder that contains `TrafficMonitor.exe`.

The folder then looks like this:

```text
TrafficMonitor
├─ TrafficMonitor.exe
└─ plugins
   ├─ ClaudeUsagePlugin.dll
   └─ ClaudeUsagePlugin
      ├─ claude-web-helper.ps1
      ├─ codex-usage-helper.ps1
      ├─ helper-common.ps1
      ├─ LICENSE, NOTICE.md, PRIVACY.md
      └─ helper
         ├─ claude-web-helper   (index.mjs, package.json, package-lock.json)
         └─ codex-usage-helper  (index.mjs, lib.mjs, package.json)
```

### 4. Start TrafficMonitor and show the taskbar window

Start `TrafficMonitor.exe`. The plugin starts both helpers in the background by itself.

<p align="center">
  <img src="images/trafficmonitor-tray-icon.png" alt="TrafficMonitor floating window and tray icon" />
</p>

If the taskbar window is not visible, right-click the tray icon or the floating window and choose `Show Taskbar Window`.

<p align="center">
  <img src="images/trafficmonitor-tray-menu-show-taskbar.png" alt="TrafficMonitor tray context menu with Show Taskbar Window" />
</p>

### 5. Enable the items

Right-click the taskbar window and choose `Display Settings...`.

<p align="center">
  <img src="images/trafficmonitor-taskbar-menu-display-settings.png" alt="TrafficMonitor taskbar context menu with Display Settings" />
</p>

Check the items you want, then click `OK`:

- `Claude 5h`, `Claude 7d`, `Codex 5h`, `Codex 7d`: used percentage of each limit
- Optional: `Claude Fable 7d` (Fable weekly limit), `Codex credits` (credits balance), `Claude resets` and `Codex resets` (usage-limit resets held)

<p align="center">
  <img src="images/trafficmonitor-display-settings.png" alt="TrafficMonitor Display settings with Claude and Codex usage items enabled" />
</p>

### 6. Sign in to Claude once

Open PowerShell in the TrafficMonitor folder (for example `cd D:\TrafficMonitor`) and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 login
```

A browser window opens with the helper's own profile. Sign in to Claude there, then close that window. The running helper picks up the sign-in on its next refresh, within 5 minutes, and keeps it across updates.

### 7. Codex in a different folder

Codex state is read from `%USERPROFILE%\.codex`. If yours lives elsewhere, set the `CODEX_HOME` environment variable in Windows before starting TrafficMonitor.

## Update to a new version

The sign-in, settings and enabled items stay as they are.

1. Exit TrafficMonitor (right-click the tray icon, `Exit`). The plugin DLL cannot be replaced while it runs.
2. Stop both helpers, which keep running after TrafficMonitor exits. In PowerShell, from the TrafficMonitor folder:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 stop
   powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\codex-usage-helper.ps1 stop
   ```

3. Extract the new zip into the TrafficMonitor folder and overwrite the existing files.
4. Start TrafficMonitor. The helpers start again with the new files.

## Check the install

- TrafficMonitor's plug-in management lists `AI Usage Limits` with the version from the zip name.
- The taskbar items show numbers instead of `--`. Claude needs the sign-in; Codex needs recent Codex activity or a server answer.
- Hovering over the taskbar window shows the Claude, Codex and Reset Credits sections.
- The helper status commands report a running watcher and a recent snapshot:

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 status
  powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\codex-usage-helper.ps1 status
  ```

If something is off, see [troubleshooting.md](troubleshooting.md).

## Recommended TrafficMonitor settings

- In `General settings`, enable `Auto run when Windows starts` so the taskbar items come back after sign-in.
- Keep `Show Taskbar Window` enabled.

## Uninstall

1. Exit TrafficMonitor and stop both helpers as in [Update to a new version](#update-to-a-new-version).
2. Delete `plugins\ClaudeUsagePlugin.dll` and the `plugins\ClaudeUsagePlugin` folder.
3. To remove local data, including the Claude sign-in cookies, delete `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin` ([PRIVACY.md](../PRIVACY.md)).
