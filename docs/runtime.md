# Runtime and Helper Guide

## Runtime Model

Claude usage limits:

- Reads a fresh helper snapshot from `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-web-usage.json`
- The helper signs in through its own local Edge or Chrome profile, reads the saved Claude cookies from that profile, and calls `https://claude.ai/api/organizations/{lastActiveOrg}/usage`
- The helper refreshes every 5 minutes by default (`claude_refresh_minutes`, minimum 1 minute) and honors `Retry-After` on HTTP 429
- While Claude Code is in use, the helper refreshes sooner: it watches the transcript folder (`%USERPROFILE%\.claude\projects`, or `CLAUDE_CONFIG_DIR\projects`) and fetches 5 seconds after new activity, at most once a minute. It only notices that a `.jsonl` file changed; it never reads the transcripts. Rate limits and sign-in problems keep the regular schedule, and usage from claude.ai or the Claude app alone is picked up on the regular refresh
- The plugin checks the snapshot's write time every 5 seconds and shows a new snapshot right away
- A rate-limited or failed request keeps the last snapshot; the tooltip shows its age and the helper status
- Snapshots older than two refresh intervals are drawn dimmed and marked stale; snapshots older than 30 minutes are dropped and Claude shows unavailable

Codex usage limits:

- Reads the Codex usage helper snapshot from `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\codex-usage.json` first; the file is re-read only when its write time changes (checked every 5 seconds)
- The helper (`codex-usage-helper.ps1`) gets the `codex` limit bucket from, in order:
  1. `codex app-server` `account/rateLimits/read` (Codex handles token refresh)
  2. `GET https://chatgpt.com/backend-api/wham/usage` with the ChatGPT token in `auth.json` (read-only; the helper never refreshes or writes `auth.json`)
  3. Session JSONL `token_count` events
- None of these sources sends a model request or spends model tokens. The helper sends only `initialize` and `account/rateLimits/read` to the app-server and calls only the `wham/usage` URL; an allowlist rejects anything else
- Push: the helper watches `sessions` with a file-system watcher and applies a new `codex` event with real windows immediately
- Server requests: every 15 minutes (`codex_server_refresh_minutes`, minimum 30 seconds), shortly after a known `resets_at`, and when a session reports the usage limit; these extra requests stay 5 minutes apart from the previous request. Failures back off up to 60 minutes and HTTP 429 `Retry-After` is honored
- When the helper snapshot is missing or older than 30 minutes, the plugin scans session JSONL itself:
  - only `limit_id: "codex"` events with a non-null window are used (`premium`, model-specific buckets, and Anthropic-routed turns with null windows are ignored)
  - the newest event is chosen by its own timestamp, not by file modification time. Codex keeps session files open while appending, and Windows does not advance their modification time until the file is closed
  - the 32 most recently opened files are scanned from the end (up to 16 MB each, so large sessions are no longer skipped); later scans read only appended bytes
- Classifies a 300-minute window (±1) as `X5h` and a 10080-minute window (±1) as `X7d` regardless of `primary` / `secondary` position; the legacy mapping is used only when `window_minutes` is absent
- The tooltip shows data age, source, plan and whether the limit is reached. Values older than 30 minutes, or whose reset time has passed, are drawn dimmed and marked in the tooltip
- When the server reports free rate limit resets, the tooltip adds `Reset credits: <count>` with the earliest expiry (the app-server reports expiry; `wham/usage` reports only the count). Session JSONL has no reset credits, so the last server value is kept
- Respects `CODEX_HOME` when it resolves to a Windows-readable path, including WSL-style `/mnt/c/...` paths

## Helper Settings

Optional settings live in `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\helper-config.json`:

```json
{
  "node_path": "C:\\Program Files\\nodejs\\node.exe",
  "claude_refresh_minutes": 5,
  "codex_server_refresh_minutes": 15,
  "codex_path": null
}
```

- `node_path`: both helpers run with this Node.js (22+). PATH is not consulted, so another app's bundled `node.exe` is never picked up. When unset, the first working standard install (`%ProgramFiles%\nodejs`, `%ProgramFiles(x86)%\nodejs`, `%LOCALAPPDATA%\Programs\nodejs`) is pinned here. `TRAFFICMONITOR_AI_USAGE_NODE` overrides it
- `codex_path`: Codex executable for the app-server request. When unset: `codex.exe` on PATH, then the newest `%LOCALAPPDATA%\OpenAI\Codex\bin\<hash>\codex.exe`

## `CODEX_HOME` Notes

- If `CODEX_HOME` is not set, the plugin uses `%USERPROFILE%\.codex`
- Set `CODEX_HOME` if your Codex state lives somewhere else
- The plugin reads the `sessions\**\*.jsonl` tree under that directory
- Windows path example: `C:\Users\<user>\.codex`
- WSL path example: `/mnt/c/Users/<user>/.codex`
- Linux-only paths such as `/home/<user>/.codex` are not readable from Windows TrafficMonitor

## Claude Helper Files

- Dedicated browser profile: `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-browser-profile`
- Usage snapshot: `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-web-usage.json`
- Helper status: `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-web-helper-status.json`
- Watch lock: `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-web-helper-watch.lock`

## Claude Helper Prerequisites

- Windows
- Node.js 22 or newer at a standard install location or `node_path` in `helper-config.json` (PATH is not used)
- Microsoft Edge or Google Chrome installed locally

## Claude Helper Commands

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\claude-web-helper.ps1 login
```

- Opens a browser window with the helper's dedicated local profile
- Sign in to Claude there, then close that helper browser window
- In a deployed TrafficMonitor install, the bundled script path is `.\plugins\ClaudeUsagePlugin\claude-web-helper.ps1`

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\claude-web-helper.ps1 start
```

- Launches the background refresh loop as a hidden background process
- This is the normal steady-state mode after the one-time login
- If a watcher is already running, it prints the current watcher PID instead of starting a duplicate
- A persisted watch lock is accepted only when its PID still belongs to a matching Node `index.mjs watch` process with the same start time
- `stop` terminates only that validated watcher; a reused PID owned by another process is never terminated

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\claude-web-helper.ps1 status
```

- Shows the latest helper files, watch lock state, and helper process information

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\claude-web-helper.ps1 stop
```

- Stops the running helper watcher and cleans up a stale watch lock when possible

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\claude-web-helper.ps1 watch
```

- Repeats the cookie-based web fetch every 5 minutes by default (about once a minute while Claude Code is active) in the foreground
- Useful only when you want console output for each refresh attempt

## Codex Helper

Files under `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\`:

- Usage snapshot: `codex-usage.json` (`source`, `method`, `data_at`, `plan_type`, `rate_limit_reached_type`, `limit_reached`, `five_hour`, `seven_day`, `reset_credits` = `{ available_count, earliest_expires_at }` or null)
- Helper status: `codex-usage-helper-status.json` (server attempts, next request time, `Retry-After`, last error)
- Watch lock: `codex-usage-helper-watch.lock`

Commands (deployed path: `.\plugins\ClaudeUsagePlugin\codex-usage-helper.ps1`):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\codex-usage-helper.ps1 once    # one server request, JSONL fallback
powershell -ExecutionPolicy Bypass -File .\scripts\codex-usage-helper.ps1 start   # hidden background watcher
powershell -ExecutionPolicy Bypass -File .\scripts\codex-usage-helper.ps1 status  # snapshot, status, lock, Node.js path
powershell -ExecutionPolicy Bypass -File .\scripts\codex-usage-helper.ps1 stop
```

The plugin starts the Codex watcher on load, the same way as the Claude watcher.


## Operational Notes

- `login` is the only interactive step
- If `claude-web-helper.ps1` plus `helper\claude-web-helper\...` are bundled under `plugins\ClaudeUsagePlugin`, the plugin can auto-start the helper watcher on plugin load
- Close the helper browser window before `start` or `watch`, otherwise the Chromium cookies database may stay locked
- The helper decrypts Chromium cookies under the same Windows user that completed the helper login
- The helper uses only Node built-ins under `helper\claude-web-helper`; no Playwright install is required
- If helper auth expires, the helper status file records the last failure and Claude becomes unavailable after the freshness window expires

## Privacy Notes

- The Claude helper profile contains Claude browser cookies for the dedicated helper login
- The Claude helper snapshot and status files can contain usage data, organization identifiers or names, file paths, and error text
- Codex session JSONL files can contain sensitive session content; the plugin scans them locally for rate-limit payloads
- Do not share helper files, helper browser profiles, or Codex session JSONL files without reviewing and sanitizing them first

See [../PRIVACY.md](../PRIVACY.md) for the full local-data disclosure.

## Refresh Behavior

- Claude helper watch refresh: 5 minutes (configurable, minimum 1 minute); Claude Code activity pulls it forward to 5 seconds after the activity, at most once a minute; 429 waits for `Retry-After` or backs off up to 60 minutes
- Claude plugin refresh: snapshot write-time check every 5 seconds, full reload every 30 seconds; snapshot stale after two helper intervals, dropped after 30 minutes
- Codex helper: session JSONL push (immediate); server every 15 minutes plus reset and usage-limit triggers (5 minutes apart); failure backoff up to 60 minutes; 429 `Retry-After`
- Codex plugin: helper snapshot write-time check every 5 seconds; its own JSONL scan every 60 seconds only while the helper snapshot is missing or older than 30 minutes
