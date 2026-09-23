# Privacy and Local Data

This plugin is a local TrafficMonitor integration. It does not run project
telemetry or send usage data to a service operated by this repository.

## Claude Web Helper

Claude values come from the bundled helper, not from an official public
Anthropic plugin API.

- `claude-web-helper.ps1 login` opens Edge or Chrome with a dedicated local
  browser profile at
  `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-browser-profile`.
- That profile stores Claude browser cookies after the user signs in.
- The helper reads and decrypts cookies from that dedicated profile under the
  same Windows user account.
- The helper sends cookie-authenticated requests to `https://claude.ai` for the
  active organization's usage data.
- The helper writes local status and usage snapshot files under
  `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin`.
- To refresh sooner while Claude Code is in use, the helper watches the Claude
  Code transcript folder (`%USERPROFILE%\.claude\projects`, or
  `CLAUDE_CONFIG_DIR\projects`) for file-change notifications. It only uses the
  fact that a `.jsonl` file changed; it never opens or reads the transcripts.

Local Claude helper files can include usage-limit data, organization
identifiers or names, local file paths, and troubleshooting error text. Do not
share the helper browser profile, helper status JSON, or helper usage snapshot
unless you have reviewed them first.

To remove the local Claude helper data, stop the helper watcher, close
TrafficMonitor, then delete
`%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin`.

## Codex Usage Helper and Session Files

Codex values come from the bundled Codex usage helper and, as a fallback, from
local Codex session JSONL files:

- Default path: `%USERPROFILE%\.codex\sessions\**\*.jsonl`
- Override path: `CODEX_HOME\sessions\**\*.jsonl`

Codex session files can contain sensitive session content, prompts, outputs,
local paths, model metadata, tool metadata, and rate-limit events. The plugin
and the helper open these files locally and scan only for rate-limit payloads.
They do not upload Codex session files.

The Codex usage helper:

- starts a short-lived `codex app-server` and sends only `initialize` and
  `account/rateLimits/read`. It never starts a thread or turn, so no model
  request is made and no model tokens are used.
- as a fallback, reads the ChatGPT access token and account id from
  `CODEX_HOME\auth.json` and sends them only to
  `https://chatgpt.com/backend-api/wham/usage`. It never refreshes, prints, or
  writes the token.
- writes `codex-usage.json` (usage percentages, reset times, plan type) and
  `codex-usage-helper-status.json` (request times, error text, local paths)
  under `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin`.

Do not attach Codex session JSONL files to bug reports or release artifacts
unless they have been sanitized.

## Network Behavior

- The Claude helper makes network requests to `https://claude.ai` when fetching
  Claude usage data.
- The Codex usage helper requests Codex usage limits through `codex app-server`
  (which talks to OpenAI) and, as a fallback, from
  `https://chatgpt.com/backend-api/wham/usage`. The plugin DLL itself only reads
  local files.
- TrafficMonitor itself, Windows, Edge, Chrome, Claude, Codex, and related
  services may have their own network behavior outside this plugin.

## Release Review

Review this document before each release against the current runtime behavior,
helper files, and release asset contents.
