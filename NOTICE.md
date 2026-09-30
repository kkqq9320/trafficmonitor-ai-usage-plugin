# Notices

## Fork

This repository is a fork of [bemaru/trafficmonitor-ai-usage-plugin](https://github.com/bemaru/trafficmonitor-ai-usage-plugin), based on its v0.3.13 (`b6dfeac`). Changes after that commit are by kkqq9320 and are listed in [CHANGELOG.md](CHANGELOG.md). The original copyright notice and license in [LICENSE](LICENSE) apply to the whole project.

## TrafficMonitor

This project builds a plugin for [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor).
TrafficMonitor itself is not bundled in this repository.

## TrafficMonitor Interface

- Upstream project: [zhongyang219/TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)
- Upstream license file: [TrafficMonitor LICENSE](https://github.com/zhongyang219/TrafficMonitor/blob/master/LICENSE)
- Included interface file: `include/PluginInterface.h`
- Local provenance: copied from TrafficMonitor's `include/PluginInterface.h`
- Header notice: `TrafficMonitor` plugin interface, copyright Zhong Yang 2021

Keep the upstream copyright header in `include/PluginInterface.h`.
If the interface is refreshed from upstream, update this notice with the
source commit or tag used for the copy.

The upstream TrafficMonitor license is the Anti-996 License Version 1.0
(Draft). This repository uses the same license for the project and keeps the
upstream interface copyright notice in `include/PluginInterface.h`.

## Bundled Helper Runtime

The Claude web helper (`helper/claude-web-helper`) and the Codex usage helper
(`helper/codex-usage-helper`) use Node.js built-in modules only. Neither has
third-party npm runtime packages.

## Service Names

Claude, Anthropic, Codex, OpenAI, Windows, Microsoft Edge, Google Chrome, and
TrafficMonitor are names of their respective owners. This project is an
unofficial integration and is not endorsed by Anthropic, OpenAI, Microsoft,
Google, or the TrafficMonitor project.
