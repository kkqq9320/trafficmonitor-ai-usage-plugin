# Troubleshooting

## Quick Verification

After installation or setup, check the following:

1. TrafficMonitor plug-in management shows `AI Usage Limits`
2. Display settings lists `Claude 5h`, `Claude 7d`, `Codex 5h`, and `Codex 7d`
3. The taskbar items show used percentages instead of `--`
4. The tooltip shows used percentages and reset timing for any source that exposes reset metadata
5. If Claude helper is enabled, `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-web-usage.json` updates after a successful helper fetch

## Common Issues

### Claude values show `--` or `Claude usage limits unavailable`

Verify that `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-web-usage.json` exists and was updated recently by the helper.
The Claude tooltip also surfaces the latest helper status when no fresh helper snapshot is available.

```powershell
powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 status
```

That should show a healthy watcher and recent files.

### Claude helper status shows `login_required` or `access_denied`

Run the Claude login again and complete the login in the opened browser window:

```powershell
powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 login
```

### Claude helper status shows `profile_in_use`

Close the helper browser window that was opened by `login`, then run `start` again.
Use `watch` only for foreground troubleshooting.

### Claude helper reports an old watcher after Windows restarts

Plugin `v0.3.13` and newer validates the process behind
`claude-web-helper-watch.lock` by name, command line, and start time. If Windows
reused the recorded PID for another process, `status` or `start` removes the
stale lock without terminating that unrelated process.

```powershell
powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 status
powershell -ExecutionPolicy Bypass -File .\plugins\ClaudeUsagePlugin\claude-web-helper.ps1 start
```

### Claude helper status shows `rate_limited` or `request_failed`

The helper could not fetch `claude.ai` usage right now.
The helper keeps the last snapshot, waits for `Retry-After` (or backs off up to 60 minutes), and the tooltip shows the snapshot age.
After 30 minutes without a successful refresh Claude becomes unavailable.

### Claude usage limits do not match the Claude web dashboard

Claude uses the helper snapshot as its Claude source.
Verify that the helper is updating `%LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\claude-web-usage.json` and compare that file to the Claude web dashboard.

### Codex usage does not appear

Check these first:

- `codex-usage-helper.ps1 status` shows a recent snapshot and a running watcher
- The helper can find Node.js 22+ (`node_path` in `helper-config.json`) and a Codex executable
- `CODEX_HOME` or `%USERPROFILE%\.codex` resolves from the Windows TrafficMonitor process
- The resolved path is Windows-readable
- At least one `sessions\**\*.jsonl` file exists under that directory

### Codex value is dimmed or the tooltip says `(stale)`

The shown data is older than 30 minutes, or its reset time has already passed.
This happens when the Codex usage helper is not running and the plugin falls back to session JSONL.
Codex turns routed to another provider (for example through a proxy) write the `codex` bucket with null windows, so they never refresh the local value.
Start the helper with `codex-usage-helper.ps1 start`.

### Codex percentage looks inverted from the Codex usage page

TrafficMonitor now standardizes Codex on used percentage in both the widget and tooltip.
If the Codex usage page is showing remaining percentage for the same window, the two numbers should add up to about 100%.
If the numbers are not simple inverses, verify that `%USERPROFILE%\.codex\sessions\**\*.jsonl` is being updated and that `CODEX_HOME` points at the same Codex profile the dashboard is using.

### A weekly Codex limit appears under `X5h`

Current Codex payloads can place a seven-day limit in `primary` instead of `secondary`.
Plugin `v0.3.12` and newer classify limits by their reported duration:

- `300` minutes is displayed as `X5h`
- `10080` minutes is displayed as `X7d`

If a current payload is still shown under the wrong label, verify that TrafficMonitor loaded `v0.3.12` or newer and restart TrafficMonitor after replacing the DLL.

### Codex values jump between different weekly reset times

Session JSONL can contain several limit buckets (`codex`, `premium`, model-specific buckets).
The plugin and helper use only the `codex` bucket and pick the newest event by its own timestamp.
File modification times are not used for this because Windows does not update them while Codex keeps a session file open.

### `Codex config directory not found`

`CODEX_HOME` or `%USERPROFILE%\.codex` could not be resolved from the Windows TrafficMonitor process.

### `Codex sessions JSONL not found`

No session JSONL files were found under the resolved Codex config directory.
Verify `CODEX_HOME`, Windows path visibility, and that Codex has started at least one session on that profile.

### `Codex sessions JSONL has no codex rate limits yet`

Codex local session logs were found, but none of the recent ones contains a `codex` rate-limit event with a real window.
Start the Codex usage helper so the value comes from the server.

### Plug-in loads but the items do not appear

This is usually one of these:

- DLL architecture does not match TrafficMonitor architecture
- The items are not enabled yet in `Display Settings...`

## Constraints

- The Claude web helper depends on an interactive Claude web login stored in its dedicated local Chromium profile
- Claude values depend on a fresh helper snapshot
- The Claude web helper is not a separate installer or Windows service; it is shipped as bundled files under `plugins\ClaudeUsagePlugin`
- Codex usage comes from `codex app-server` / `wham/usage` through the helper; these are the endpoints Codex itself uses, not a documented public API
- Without the helper, Codex values update only after Codex itself writes fresh rate-limit data into session JSONL files
- There is no SQLite fallback for Codex usage
- This is a best-effort integration surface, not an official Anthropic or OpenAI plugin
