# TrafficMonitor AI Usage Limits v0.4.0-kkqq.1

Claude and Codex usage limits in the Windows taskbar through TrafficMonitor. First release of the kkqq9320 fork, based on [bemaru v0.3.13](https://github.com/bemaru/trafficmonitor-ai-usage-plugin/releases/tag/v0.3.13).

## What Changed

- **Codex usage helper:** reads the Codex limits from `codex app-server`, then `wham/usage`, then session logs, and updates as soon as Codex writes a new event. It never sends a model request.
- **New optional taskbar items:** `Claude Fable 7d` (`CF7d`), `Codex credits` (`Xcr`, whole balance), `Claude resets` (`Crs`) and `Codex resets` (`Xrs`).
- **New tooltip layout:** one section per service with the percentage left, time until each reset, local reset time with the weekday, plan and credits, then a `Reset Credits` section.
- **Claude refresh:** refreshes within about a minute while Claude Code is in use, every 5 minutes otherwise, and honors `Retry-After`.
- **Fixes:** Codex no longer shows an old percentage from a session file Windows kept open, ignores other limit buckets, and reads session files over 32 MB. Cookie decryption works on Windows PowerShell 5.1.
- **Plugin info:** author `kkqq9320`, link to this fork.

The full list is in [CHANGELOG.md](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin/blob/main/CHANGELOG.md).

## Install or Update

- Asset: `TrafficMonitorAIUsageLimits_v0.4.0-kkqq.1_x64.zip` for the x64 build of TrafficMonitor.
- New install: extract the zip into the folder that contains `TrafficMonitor.exe`, then follow the [install guide](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin/blob/main/docs/install.md) ([한국어](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin/blob/main/docs/install.ko.md)).
- Update from bemaru v0.3.x: exit TrafficMonitor, stop the Claude helper, extract over the existing files, start TrafficMonitor ([update steps](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin/blob/main/docs/install.md#update-to-a-new-version)). The Codex helper is new, so its stop command is not needed the first time.

## Requirements

- Windows x64, TrafficMonitor x64, Node.js 22 or newer
- Microsoft Edge or Google Chrome for the one-time Claude sign-in
- Codex signed in on the same PC for Codex values

This is an unofficial integration, not an Anthropic, OpenAI or TrafficMonitor project.
