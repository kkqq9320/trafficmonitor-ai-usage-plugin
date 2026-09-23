// Codex usage helper for the TrafficMonitor AI Usage Limits plugin.
//
// Writes %LOCALAPPDATA%\trafficmonitor-claude-usage-plugin\codex-usage.json for the plugin DLL.
// Sources (see lib.mjs): codex app-server account/rateLimits/read -> wham/usage -> session JSONL.
// The helper never starts a thread or turn and never calls a model endpoint.
//
// Modes:
//   once   one server request (falling back to session JSONL), then exit
//   watch  keep running: push updates from session JSONL + scheduled server requests
//   status print the current snapshot and helper status

import fs from 'node:fs';
import fsp from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';
import { spawn, spawnSync } from 'node:child_process';
import * as lib from './lib.mjs';

const MODE = (process.argv[2] || 'once').toLowerCase();
const BASE_DIR = process.env.LOCALAPPDATA
  ? path.join(process.env.LOCALAPPDATA, 'trafficmonitor-claude-usage-plugin')
  : path.join(os.homedir(), '.cache', 'trafficmonitor-claude-usage-plugin');
const SNAPSHOT_PATH = path.join(BASE_DIR, 'codex-usage.json');
const STATUS_PATH = path.join(BASE_DIR, 'codex-usage-helper-status.json');
const WATCH_LOCK_PATH = path.join(BASE_DIR, 'codex-usage-helper-watch.lock');
const CONFIG_PATH = path.join(BASE_DIR, 'helper-config.json');
const HELPER_MARKER = 'codex-usage-helper';

const APP_SERVER_INIT_TIMEOUT_MS = 30_000;
const APP_SERVER_REQUEST_TIMEOUT_MS = 20_000;
const HTTP_TIMEOUT_MS = 10_000;
const TICK_MS = 15_000;
const JSONL_CHUNK_BYTES = 1024 * 1024;
const JSONL_TAIL_MAX_BYTES = 16 * 1024 * 1024;
// Codex keeps session files open while it appends, and Windows does not advance their mtime until
// the handle closes. mtime therefore only approximates "recently opened", so the scan reads the
// newest events of the most recently opened files and compares event timestamps.
const JSONL_SCAN_MAX_FILES = 32;
const JSONL_FIRST_SEEN_TAIL_BYTES = 256 * 1024;
const JSONL_DEBOUNCE_MS = 400;

function normalizeWslPath(value) {
  const match = /^\/mnt\/([a-zA-Z])\/(.*)$/.exec(value || '');
  return match ? match[1].toUpperCase() + ':\\' + match[2].replace(/\//g, '\\') : value;
}

function getCodexHome() {
  const override = (process.env.CODEX_HOME || '').trim();
  return override ? normalizeWslPath(override) : path.join(os.homedir(), '.codex');
}

const CODEX_HOME = getCodexHome();
const SESSIONS_DIR = path.join(CODEX_HOME, 'sessions');
const AUTH_PATH = path.join(CODEX_HOME, 'auth.json');

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function readJsonFileSync(filePath) {
  try {
    return JSON.parse(fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, ''));
  } catch {
    return null;
  }
}

function loadConfig() {
  const config = readJsonFileSync(CONFIG_PATH) || {};
  const envRefresh = Number(process.env.CODEX_USAGE_HELPER_REFRESH_MS);
  const configRefresh = Number(config.codex_server_refresh_minutes) * 60_000;
  return {
    codexPath: (process.env.CODEX_USAGE_HELPER_CODEX_PATH || config.codex_path || '').trim() || null,
    serverRefreshMs: lib.clampServerRefreshMs(Number.isFinite(envRefresh) && envRefresh > 0 ? envRefresh : configRefresh),
  };
}

async function atomicWriteJson(filePath, value) {
  await fsp.mkdir(path.dirname(filePath), { recursive: true });
  const tempPath = filePath + '.' + process.pid + '.tmp';
  await fsp.writeFile(tempPath, JSON.stringify(value, null, 2) + '\n', 'utf8');
  for (let attempt = 0; ; attempt += 1) {
    try {
      await fsp.rename(tempPath, filePath);
      return;
    } catch (error) {
      // The plugin may be reading the file at this moment.
      if (attempt >= 5 || !error || !['EPERM', 'EBUSY', 'EACCES'].includes(error.code)) {
        await fsp.unlink(tempPath).catch(() => {});
        throw error;
      }
      await delay(100 * (attempt + 1));
    }
  }
}

// ---------------------------------------------------------------------------
// Codex executable discovery

function listPathDirs() {
  return String(process.env.PATH || '')
    .split(';')
    .map((entry) => entry.trim().replace(/^"(.*)"$/, '$1'))
    .filter(Boolean);
}

function listCodexAppBinDirs() {
  const root = process.env.LOCALAPPDATA ? path.join(process.env.LOCALAPPDATA, 'OpenAI', 'Codex', 'bin') : null;
  if (!root || !fs.existsSync(root)) return [];
  const result = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const dir = path.join(root, entry.name);
    try {
      result.push({ dir, exeMtimeMs: fs.statSync(path.join(dir, 'codex.exe')).mtimeMs });
    } catch {
      // hash folders without codex.exe are skipped
    }
  }
  return result;
}

function findCodexExecutable(config) {
  return lib.resolveCodexExecutable({
    configuredPath: config.codexPath,
    pathDirs: listPathDirs(),
    appBinDirs: listCodexAppBinDirs(),
    exists: (candidate) => {
      try {
        return fs.statSync(candidate).isFile();
      } catch {
        return false;
      }
    },
  });
}

// ---------------------------------------------------------------------------
// Source 1: codex app-server account/rateLimits/read

function killProcessTree(child) {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  if (process.platform === 'win32' && child.pid) {
    spawnSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], { windowsHide: true, stdio: 'ignore' });
  } else {
    child.kill('SIGKILL');
  }
}

export function readRateLimitsViaAppServer(codexPath) {
  return new Promise((resolve, reject) => {
    const args = ['-c', 'approval_policy="never"', '-c', 'features.plugins=false', '-s', 'read-only', '-a', 'never', 'app-server'];
    let child;
    try {
      child = spawn(codexPath, args, { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true, cwd: os.tmpdir() });
    } catch (error) {
      reject(error);
      return;
    }

    let settled = false;
    let stderr = '';
    let nextId = 0;
    let initId = null;
    let readId = null;
    let timer = null;

    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      killProcessTree(child);
      if (error) reject(error);
      else resolve(value);
    };
    const arm = (ms, label) => {
      clearTimeout(timer);
      timer = setTimeout(() => finish(new Error('app-server ' + label + ' timed out')), ms);
    };
    const sendRequest = (method, params) => {
      lib.assertAllowedAppServerRequest(method);
      nextId += 1;
      child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: nextId, method, params: params ?? {} }) + '\n');
      return nextId;
    };
    const sendNotification = (method) => {
      lib.assertAllowedAppServerNotification(method);
      child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params: {} }) + '\n');
    };

    child.on('error', (error) => finish(error));
    child.stdin.on('error', (error) => finish(error));
    child.stderr.on('data', (chunk) => {
      stderr = (stderr + chunk.toString()).slice(-4000);
    });
    child.on('close', (code) => {
      const detail = stderr.trim().split(/\r?\n/).slice(-3).join(' | ');
      finish(new Error('app-server exited (' + code + ')' + (detail ? ': ' + detail : '')));
    });

    readline.createInterface({ input: child.stdout }).on('line', (line) => {
      let message;
      try {
        message = JSON.parse(line);
      } catch {
        return;
      }
      if (message.id === undefined || message.id === null) return;
      try {
        if (message.id === initId) {
          if (message.error) {
            finish(new Error('app-server initialize failed: ' + (message.error.message || 'unknown error')));
            return;
          }
          sendNotification('initialized');
          readId = sendRequest('account/rateLimits/read');
          arm(APP_SERVER_REQUEST_TIMEOUT_MS, 'account/rateLimits/read');
        } else if (message.id === readId) {
          if (message.error) {
            finish(new Error('account/rateLimits/read failed: ' + (message.error.message || 'unknown error')));
            return;
          }
          finish(null, lib.normalizeAppServerRateLimits(message.result));
        }
      } catch (error) {
        finish(error);
      }
    });

    arm(APP_SERVER_INIT_TIMEOUT_MS, 'initialize');
    try {
      initId = sendRequest('initialize', { clientInfo: { name: 'trafficmonitor_ai_usage', title: 'TrafficMonitor AI Usage Limits', version: '0.4.0' } });
    } catch (error) {
      finish(error);
    }
  });
}

// ---------------------------------------------------------------------------
// Source 2: GET chatgpt.com/backend-api/wham/usage (read-only; never refreshes or writes auth.json)

export async function readRateLimitsViaWham(fetchImpl = fetch) {
  lib.assertAllowedHttpUrl(lib.WHAM_USAGE_URL);
  const auth = readJsonFileSync(AUTH_PATH);
  const accessToken = auth && auth.tokens && auth.tokens.access_token;
  if (!accessToken) {
    const error = new Error('No ChatGPT access token in ' + AUTH_PATH);
    error.httpStatus = 401;
    throw error;
  }
  const headers = {
    accept: 'application/json',
    authorization: 'Bearer ' + accessToken,
    'user-agent': 'codex-cli',
  };
  if (auth.tokens.account_id) headers['chatgpt-account-id'] = auth.tokens.account_id;

  const response = await fetchImpl(lib.WHAM_USAGE_URL, { headers, signal: AbortSignal.timeout(HTTP_TIMEOUT_MS) });
  if (!response.ok) {
    const error = new Error('wham/usage HTTP ' + response.status);
    error.httpStatus = response.status;
    error.retryAfterMs = lib.parseRetryAfter(response.headers.get('retry-after'), Date.now());
    await response.body?.cancel().catch(() => {});
    throw error;
  }
  return lib.normalizeWhamUsage(await response.json());
}

// ---------------------------------------------------------------------------
// Source 3: session JSONL

function listSessionFiles() {
  const files = [];
  const walk = (dir) => {
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) walk(full);
      else if (entry.name.toLowerCase().endsWith('.jsonl')) {
        try {
          const stat = fs.statSync(full);
          files.push({ path: full, mtimeMs: stat.mtimeMs, size: stat.size });
        } catch {
          // file vanished
        }
      }
    }
  };
  walk(SESSIONS_DIR);
  return files.sort((a, b) => b.mtimeMs - a.mtimeMs);
}

// Reads complete lines from the end of a file, newest first, without loading the whole file.
function forEachLineFromEnd(filePath, maxBytes, visit) {
  const fd = fs.openSync(filePath, 'r');
  try {
    const size = fs.fstatSync(fd).size;
    let end = size;
    let carry = Buffer.alloc(0);
    let scanned = 0;
    while (end > 0 && scanned < maxBytes) {
      const start = Math.max(0, end - JSONL_CHUNK_BYTES);
      const chunk = Buffer.alloc(end - start);
      fs.readSync(fd, chunk, 0, chunk.length, start);
      scanned += chunk.length;
      let data = Buffer.concat([chunk, carry]);
      let lineEnd = data.length;
      for (let index = data.length - 1; index >= 0; index -= 1) {
        if (data[index] !== 0x0a) continue;
        if (lineEnd > index + 1 && visit(data.subarray(index + 1, lineEnd).toString('utf8'))) return;
        lineEnd = index;
      }
      carry = data.subarray(0, lineEnd);
      end = start;
      if (start === 0 && carry.length && visit(carry.toString('utf8'))) return;
    }
  } finally {
    fs.closeSync(fd);
  }
}

export function findLatestJsonlEvent() {
  let best = null;
  for (const file of listSessionFiles().slice(0, JSONL_SCAN_MAX_FILES)) {
    try {
      forEachLineFromEnd(file.path, JSONL_TAIL_MAX_BYTES, (line) => {
        const event = lib.parseRateLimitEventLine(line);
        if (!event) return false;
        if (!best || event.timestampMs > best.timestampMs) best = event;
        return true;
      });
    } catch {
      // unreadable file; skip
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Status and lock

async function writeStatus(state, details) {
  await atomicWriteJson(STATUS_PATH, { state, updated_at: new Date().toISOString(), ...details });
}

function runPowerShellJson(script) {
  const encoded = Buffer.from(script, 'utf16le').toString('base64');
  for (const executable of ['pwsh', 'powershell.exe']) {
    const result = spawnSync(executable, ['-NoProfile', '-EncodedCommand', encoded], { encoding: 'utf8', windowsHide: true });
    if (!result.error && result.status === 0) return JSON.parse(result.stdout.trim());
  }
  throw new Error('PowerShell process query failed');
}

function isHelperWatchRunning(watchLock) {
  const pid = Number(watchLock && watchLock.pid);
  if (!Number.isInteger(pid) || pid <= 0 || pid === process.pid) return false;
  try {
    const info = runPowerShellJson([
      '$p = Get-CimInstance Win32_Process -Filter "ProcessId = ' + pid + '" -ErrorAction SilentlyContinue',
      'if ($null -eq $p) { exit 3 }',
      "[pscustomobject]@{ name = [string]$p.Name; command_line = [string]$p.CommandLine; created_at = ([DateTimeOffset]$p.CreationDate).ToUniversalTime().ToString('o') } | ConvertTo-Json -Compress",
    ].join('\n'));
    if (String(info.name).toLowerCase() !== 'node.exe') return false;
    if (!String(info.command_line).includes(HELPER_MARKER) || !/(^|\s)watch(\s|$)/i.test(info.command_line)) return false;
    return Math.abs(Date.parse(info.created_at) - Date.parse(watchLock.started_at)) <= 30_000;
  } catch {
    return false;
  }
}

let watchLockHandle = null;

async function acquireWatchLock(refreshMs) {
  await fsp.mkdir(BASE_DIR, { recursive: true });
  for (let attempt = 0; attempt < 2; attempt += 1) {
    try {
      const handle = await fsp.open(WATCH_LOCK_PATH, 'wx');
      await handle.writeFile(JSON.stringify({ pid: process.pid, mode: 'watch', started_at: new Date().toISOString(), refresh_ms: refreshMs }, null, 2) + '\n', 'utf8');
      watchLockHandle = handle;
      return true;
    } catch (error) {
      if (!error || error.code !== 'EEXIST') throw error;
      const existing = readJsonFileSync(WATCH_LOCK_PATH);
      if (isHelperWatchRunning(existing)) {
        console.log('Codex usage helper watch already running (pid ' + existing.pid + ').');
        return false;
      }
      await fsp.unlink(WATCH_LOCK_PATH).catch(() => {});
    }
  }
  throw new Error('Codex usage helper watch lock could not be acquired');
}

function releaseWatchLock() {
  if (!watchLockHandle) return;
  try {
    fs.closeSync(watchLockHandle.fd);
  } catch {
    // ignore
  }
  try {
    fs.unlinkSync(WATCH_LOCK_PATH);
  } catch {
    // ignore
  }
  watchLockHandle = null;
}

// ---------------------------------------------------------------------------
// Helper state machine

export class CodexUsageHelper {
  constructor(config) {
    this.config = config;
    this.snapshot = readJsonFileSync(SNAPSHOT_PATH);
    const previousStatus = readJsonFileSync(STATUS_PATH) || {};
    const server = previousStatus.server || {};
    const parseMs = (value) => {
      const parsed = Date.parse(value);
      return Number.isFinite(parsed) ? parsed : null;
    };
    this.server = {
      lastAttemptMs: parseMs(server.last_attempt_at),
      lastSuccessMs: parseMs(server.last_success_at),
      consecutiveFailures: Number.isInteger(server.consecutive_failures) ? server.consecutive_failures : 0,
      retryAfterUntilMs: parseMs(server.retry_after_until),
      requestedAtMs: null,
      lastMethod: server.last_method || null,
      lastError: server.last_error || null,
      lastErrorKind: server.last_error_kind || null,
    };
    this.codex = null;
    this.inFlight = false;
    this.watchingSessions = false;
    this.watcher = null;
    this.stopped = false;
    this.fileOffsets = new Map();
    this.pendingFiles = new Set();
    this.debounceTimer = null;
    this.startedAtMs = Date.now();
  }

  nextServerFetchMs(nowMs = Date.now()) {
    return lib.computeNextServerFetchMs({
      nowMs,
      lastAttemptMs: this.server.lastAttemptMs,
      consecutiveFailures: this.server.consecutiveFailures,
      retryAfterUntilMs: this.server.retryAfterUntilMs,
      refreshMs: this.config.serverRefreshMs,
      snapshot: this.snapshot,
      requestedAtMs: this.server.requestedAtMs,
    });
  }

  async publish(record, { source, method, dataAtMs }) {
    const nowMs = Date.now();
    if (!lib.isNewerThanSnapshot(dataAtMs, this.snapshot)) return false;
    const previous = this.snapshot;
    this.snapshot = lib.buildSnapshot(record, { source, method, dataAtMs, fetchedAtMs: nowMs, previous });
    await atomicWriteJson(SNAPSHOT_PATH, this.snapshot);
    if (this.snapshot.limit_reached && !(previous && previous.limit_reached) && source === 'jsonl') {
      this.requestServerFetch('limit reached in session event');
    }
    return true;
  }

  requestServerFetch(reason) {
    const nowMs = Date.now();
    if (this.server.requestedAtMs === null || nowMs < this.server.requestedAtMs) {
      this.server.requestedAtMs = nowMs;
      console.log('Server refresh requested: ' + reason);
    }
  }

  async writeHelperStatus() {
    const nowMs = Date.now();
    const iso = (ms) => (ms === null || ms === undefined ? null : new Date(ms).toISOString());
    const state = this.server.consecutiveFailures === 0 ? 'ok' : this.server.lastErrorKind || 'request_failed';
    await writeStatus(state, {
      snapshot_path: SNAPSHOT_PATH,
      snapshot_source: this.snapshot ? this.snapshot.source : null,
      codex_path: this.codex ? this.codex.path : null,
      codex_origin: this.codex ? this.codex.origin : null,
      sessions_dir: SESSIONS_DIR,
      watching_sessions: this.watchingSessions,
      server_refresh_ms: this.config.serverRefreshMs,
      error: this.server.consecutiveFailures ? this.server.lastError : undefined,
      server: {
        last_attempt_at: iso(this.server.lastAttemptMs),
        last_success_at: iso(this.server.lastSuccessMs),
        last_method: this.server.lastMethod,
        consecutive_failures: this.server.consecutiveFailures,
        retry_after_until: iso(this.server.retryAfterUntilMs),
        next_fetch_at: iso(this.nextServerFetchMs(nowMs)),
        last_error: this.server.lastError,
        last_error_kind: this.server.lastErrorKind,
      },
    });
  }

  async fetchFromServer() {
    if (this.inFlight) return false;
    this.inFlight = true;
    const startedMs = Date.now();
    this.server.lastAttemptMs = startedMs;
    this.server.requestedAtMs = null;
    const failures = [];
    let record = null;
    let method = null;
    let rateLimited = null;
    try {
      this.codex = findCodexExecutable(this.config);
      if (this.codex) {
        try {
          record = await readRateLimitsViaAppServer(this.codex.path);
          method = 'app-server';
        } catch (error) {
          failures.push(error);
          if (lib.classifyServerError(error) === 'rate_limited') rateLimited = error;
        }
      } else {
        failures.push(new Error('codex executable not found'));
      }

      // Skip the second server call when the first one was already rate limited.
      if (!record && !rateLimited) {
        try {
          record = await readRateLimitsViaWham();
          method = 'wham';
        } catch (error) {
          failures.push(error);
          if (lib.classifyServerError(error) === 'rate_limited') rateLimited = error;
        }
      }

      if (record) {
        this.server.lastSuccessMs = Date.now();
        this.server.consecutiveFailures = 0;
        this.server.retryAfterUntilMs = null;
        this.server.lastMethod = method;
        this.server.lastError = failures.length ? 'recovered via ' + method + ' after: ' + failures.map((e) => e.message).join('; ') : null;
        this.server.lastErrorKind = null;
        await this.publish(record, { source: 'server', method, dataAtMs: this.server.lastSuccessMs });
      } else {
        this.server.consecutiveFailures += 1;
        this.server.lastError = failures.map((e) => e.message).join('; ');
        this.server.lastErrorKind = rateLimited ? 'rate_limited' : lib.classifyServerError(failures[failures.length - 1]);
        if (rateLimited) {
          const fallbackMs = Date.now() + lib.DEFAULTS.defaultRateLimitBackoffMs;
          this.server.retryAfterUntilMs = rateLimited.retryAfterMs || fallbackMs;
        }
        const event = findLatestJsonlEvent();
        if (event) await this.publish(event, { source: 'jsonl', method: 'session-scan', dataAtMs: event.timestampMs });
      }
      await this.writeHelperStatus();
      return Boolean(record);
    } finally {
      this.inFlight = false;
      console.log(
        new Date().toISOString() + ' server refresh ' + (record ? 'ok via ' + method : 'failed: ' + this.server.lastError) + ' (' + (Date.now() - startedMs) + ' ms)',
      );
    }
  }

  // --- push path: session JSONL watcher -----------------------------------

  startSessionWatcher() {
    const start = () => {
      if (this.stopped) return;
      try {
        const watcher = fs.watch(SESSIONS_DIR, { recursive: true }, (_event, filename) => {
          if (!filename || !String(filename).toLowerCase().endsWith('.jsonl')) return;
          this.pendingFiles.add(path.join(SESSIONS_DIR, String(filename)));
          clearTimeout(this.debounceTimer);
          this.debounceTimer = setTimeout(() => this.processPendingFiles().catch((e) => console.error(e)), JSONL_DEBOUNCE_MS);
        });
        watcher.on('error', (error) => {
          console.error('Session watcher error: ' + error.message);
          this.watchingSessions = false;
          watcher.close();
          setTimeout(start, 30_000).unref();
        });
        this.watcher = watcher;
        this.watchingSessions = true;
      } catch (error) {
        this.watchingSessions = false;
        console.error('Session watcher unavailable (' + error.message + '); retrying in 60s');
        setTimeout(start, 60_000).unref();
      }
    };
    start();
  }

  stopSessionWatcher() {
    this.stopped = true;
    clearTimeout(this.debounceTimer);
    if (this.watcher) this.watcher.close();
    this.watcher = null;
    this.watchingSessions = false;
  }

  readAppendedLines(filePath) {
    let fd;
    try {
      fd = fs.openSync(filePath, 'r');
    } catch {
      return [];
    }
    try {
      const stat = fs.fstatSync(fd);
      let offset = this.fileOffsets.get(filePath);
      let alignedToLine = true;
      if (offset === undefined || offset > stat.size) {
        const isNewFile = stat.birthtimeMs >= this.startedAtMs - 5000;
        offset = isNewFile ? 0 : Math.max(0, stat.size - JSONL_FIRST_SEEN_TAIL_BYTES);
        alignedToLine = offset === 0;
      }
      if (stat.size <= offset) {
        this.fileOffsets.set(filePath, offset);
        return [];
      }
      const length = Math.min(stat.size - offset, JSONL_TAIL_MAX_BYTES);
      const start = stat.size - length;
      if (start !== offset) alignedToLine = false;
      const buffer = Buffer.alloc(length);
      fs.readSync(fd, buffer, 0, length, start);
      const lastNewline = buffer.lastIndexOf(0x0a);
      if (lastNewline < 0) {
        // No complete line yet; keep the old offset so the line is read once it is finished.
        this.fileOffsets.set(filePath, alignedToLine ? offset : start);
        return [];
      }
      this.fileOffsets.set(filePath, start + lastNewline + 1);
      const lines = buffer.subarray(0, lastNewline).toString('utf8').split('\n');
      // When the read started in the middle of a line, drop that partial first line.
      if (!alignedToLine) lines.shift();
      return lines;
    } finally {
      fs.closeSync(fd);
    }
  }

  async processPendingFiles() {
    const files = [...this.pendingFiles];
    this.pendingFiles.clear();
    let newest = null;
    let usageLimitSeen = false;
    for (const filePath of files) {
      for (const line of this.readAppendedLines(filePath)) {
        const event = lib.parseRateLimitEventLine(line);
        if (event && (!newest || event.timestampMs > newest.timestampMs)) newest = event;
        if (!usageLimitSeen && lib.isUsageLimitEventLine(line)) usageLimitSeen = true;
      }
    }
    if (newest && (await this.publish(newest, { source: 'jsonl', method: 'session-event', dataAtMs: newest.timestampMs }))) {
      console.log(new Date().toISOString() + ' session event applied (' + new Date(newest.timestampMs).toISOString() + ')');
      await this.writeHelperStatus();
    }
    if (usageLimitSeen) this.requestServerFetch('usage limit error in session');
  }

  async tick() {
    if (this.inFlight) return;
    if (Date.now() >= this.nextServerFetchMs()) await this.fetchFromServer();
  }
}

// ---------------------------------------------------------------------------

async function runOnce() {
  const helper = new CodexUsageHelper(loadConfig());
  const ok = await helper.fetchFromServer();
  const snapshot = helper.snapshot;
  console.log(JSON.stringify({ server_ok: ok, snapshot }, null, 2));
  return snapshot ? 0 : 1;
}

async function runWatch() {
  const config = loadConfig();
  if (!(await acquireWatchLock(config.serverRefreshMs))) return 0;
  process.once('SIGINT', () => {
    releaseWatchLock();
    process.exit(0);
  });
  process.once('SIGTERM', () => {
    releaseWatchLock();
    process.exit(0);
  });
  process.once('exit', releaseWatchLock);

  const helper = new CodexUsageHelper(config);
  const event = findLatestJsonlEvent();
  if (event) await helper.publish(event, { source: 'jsonl', method: 'session-scan', dataAtMs: event.timestampMs });
  helper.startSessionWatcher();
  await helper.writeHelperStatus();
  console.log('Codex usage helper watching ' + SESSIONS_DIR + '; next server refresh at ' + new Date(helper.nextServerFetchMs()).toISOString());

  while (true) {
    try {
      await helper.tick();
    } catch (error) {
      console.error(error);
    }
    await delay(TICK_MS);
  }
}

function runStatus() {
  console.log(JSON.stringify({ snapshot: readJsonFileSync(SNAPSHOT_PATH), status: readJsonFileSync(STATUS_PATH), lock: readJsonFileSync(WATCH_LOCK_PATH) }, null, 2));
  return 0;
}

async function main() {
  switch (MODE) {
    case 'once':
      process.exitCode = await runOnce();
      break;
    case 'watch':
      await runWatch();
      break;
    case 'status':
      process.exitCode = runStatus();
      break;
    default:
      console.error('Unknown mode: ' + MODE + ' (use once, watch, status)');
      process.exitCode = 1;
  }
}

const invokedDirectly = Boolean(process.argv[1]) && path.resolve(process.argv[1]).toLowerCase() === fileURLToPath(import.meta.url).toLowerCase();
if (invokedDirectly) {
  main().catch(async (error) => {
    try {
      await writeStatus('crashed', { error: error && error.message ? error.message : String(error) });
    } catch {
      // ignore secondary failure
    }
    console.error(error);
    process.exitCode = 1;
  });
}
