# Release Checklist

Releases are published from the fork [kkqq9320/trafficmonitor-ai-usage-plugin](https://github.com/kkqq9320/trafficmonitor-ai-usage-plugin) for x64 only. Pass `-R kkqq9320/trafficmonitor-ai-usage-plugin` to every `gh` command so nothing goes to the original repository.

## Before tagging

1. `main` is clean and matches `origin/main`.
2. The tests in [build.md](build.md) pass: DLL scenarios for `x64` and `Win32`, the Node tests, and the helper wrapper tests.
3. The version is the same everywhere:
   - `TMI_VERSION` in `src/ClaudeUsagePlugin/ClaudeUsagePlugin.cpp`
   - the [CHANGELOG.md](../CHANGELOG.md) heading `## <version> - <date>`
   - the tag `v<version>`
   - `docs/release-notes-v<version>.md`, made from [release-notes-template.md](release-notes-template.md)
4. [README.md](../README.md), [README.ko.md](../README.ko.md) and the install guides still match the release.
5. [LICENSE](../LICENSE), [NOTICE.md](../NOTICE.md) and [PRIVACY.md](../PRIVACY.md) match the current behavior. Keep the LICENSE text unchanged.

## Package

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1 -Version <version>
```

The script refuses a version that differs from `TMI_VERSION`, rebuilds `Release|x64`, and writes `dist\TrafficMonitorAIUsageLimits_v<version>_x64.zip` with this layout:

```text
plugins
├─ ClaudeUsagePlugin.dll
└─ ClaudeUsagePlugin
   ├─ claude-web-helper.ps1
   ├─ codex-usage-helper.ps1
   ├─ helper-common.ps1
   ├─ LICENSE
   ├─ NOTICE.md
   ├─ PRIVACY.md
   └─ helper
      ├─ claude-web-helper   (index.mjs, package.json, package-lock.json)
      └─ codex-usage-helper  (index.mjs, lib.mjs, package.json)
```

## Publish

```powershell
git tag v<version>
git push origin v<version>
gh release create v<version> .\dist\TrafficMonitorAIUsageLimits_v<version>_x64.zip -R kkqq9320/trafficmonitor-ai-usage-plugin --title "TrafficMonitor AI Usage Limits v<version>" --notes-file .\docs\release-notes-v<version>.md
```

Build the zip from the tagged commit. Do not attach or rename a zip from another version.

## Smoke check

Install the published asset with the [update steps](install.md#update-to-a-new-version), then confirm:

1. TrafficMonitor's plug-in management shows `AI Usage Limits`, version `<version>`, author `kkqq9320`.
2. Display settings list `Claude 5h`, `Claude 7d`, `Codex 5h`, `Codex 7d`, `Codex credits`, `Claude resets`, `Codex resets` and `Claude Fable 7d`.
3. The tooltip shows the Claude, Codex and Reset Credits sections with local reset times.
4. `claude-web-helper.ps1 status` and `codex-usage-helper.ps1 status` report running watchers.
