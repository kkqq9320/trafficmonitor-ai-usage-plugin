# TrafficMonitor AI Usage Limits

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4)
![TrafficMonitor](https://img.shields.io/badge/TrafficMonitor-plugin-2EA043)
![Usage Sources](https://img.shields.io/badge/usage-Claude%20%2B%20Codex-0A7F5A)

Languages: English | [한국어](README.ko.md)

A [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) plugin that keeps Claude and Codex usage limits in the Windows taskbar. Inside TrafficMonitor it appears as `AI Usage Limits`.

This repository is a fork of [bemaru/trafficmonitor-ai-usage-plugin](https://github.com/bemaru/trafficmonitor-ai-usage-plugin) and is released on its own. [CHANGELOG.md](CHANGELOG.md) lists what the fork adds.

<p align="center">
  <img src="docs/images/trafficmonitor-taskbar-compact.png" alt="TrafficMonitor taskbar showing Claude and Codex usage bars" />
</p>

## What It Shows

| Display setting | Label | Value |
|---|---|---|
| `Claude 5h`, `Claude 7d` | `C5h`, `C7d` | Claude usage used in the 5-hour and 7-day windows |
| `Codex 5h`, `Codex 7d` | `X5h`, `X7d` | Codex usage used in the 5-hour and 7-day windows |
| `Claude Fable 7d` (optional) | `CF7d` | Claude Fable weekly limit used |
| `Codex credits` (optional) | `Xcr` | Codex credits balance |
| `Claude resets`, `Codex resets` (optional) | `Crs`, `Xrs` | Usage-limit resets each account holds |

Hovering over the taskbar window shows a tooltip with the percentage left, time until each reset, plan, credits and reset credits:

```text
📊 Claude left 5h 83% · 7d 17% · Fable 91%
5h: 17% (3h 42m) at 2026-10-01 04:19 Thursday
7d: 83% (2d 22h 22m) at 2026-10-03 22:59 Saturday
Fable 7d: 9% (2d 22h 22m) at 2026-10-03 22:59 Saturday
Updated: just now, Claude web helper, Plan Max (5x)

📊 Codex left 7d 14%
7d: 86% (3d 2h 43m) at 2026-10-04 03:19 Sunday
Credits: 62,500
Updated: 4m ago, Codex API, Plan Pro

🎟 Reset Credits
1 · Claude (Expires : 2026-10-23 01:00 Friday)
2 · Codex  (Expires : 2026-10-23 05:39 Friday)
```

How each value is read: [docs/runtime.md](docs/runtime.md).

## Quick Install

Requirements: Windows x64, the x64 build of TrafficMonitor, Node.js 22 or newer, and Microsoft Edge or Google Chrome for the one-time Claude sign-in. Codex values come from Codex signed in on the same PC.

1. Install Node.js if it is missing: `winget install OpenJS.NodeJS.LTS`
2. Download `TrafficMonitorAIUsageLimits_v<version>_x64.zip` from the [latest release](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin/releases/latest).
3. Exit TrafficMonitor, then extract the zip into the folder that contains `TrafficMonitor.exe`.
4. Start TrafficMonitor, right-click its taskbar window, choose `Display Settings...`, and check the items you want.
5. Sign in to Claude once. In PowerShell, from the TrafficMonitor folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 login
```

To update an existing install, follow [Update to a new version](docs/install.md#update-to-a-new-version). The full walkthrough with screenshots is in [docs/install.md](docs/install.md).

## Docs

- [Install, update and uninstall](docs/install.md)
- [Runtime, tooltip and helper settings](docs/runtime.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Build and tests](docs/build.md), [release checklist](docs/release-checklist.md)
- [Privacy and local data](PRIVACY.md)
- [Changelog](CHANGELOG.md), [license](LICENSE), [notices](NOTICE.md)

## Credits

The original plugin was written by [bemaru](https://github.com/bemaru), who accepts support through [GitHub Sponsors](https://github.com/sponsors/bemaru). This fork is maintained by [kkqq9320](https://github.com/kkqq9320) and is not an official Anthropic, OpenAI or TrafficMonitor project.
