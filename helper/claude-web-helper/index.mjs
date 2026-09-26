import crypto from 'node:crypto';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { spawn, spawnSync } from 'node:child_process';
import { DatabaseSync } from 'node:sqlite';

const MODE = (process.argv[2] || 'once').toLowerCase();
const BASE_DIR = process.env.LOCALAPPDATA
  ? path.join(process.env.LOCALAPPDATA, 'trafficmonitor-claude-usage-plugin')
  : path.join(os.homedir(), '.cache', 'trafficmonitor-claude-usage-plugin');
const PROFILE_DIR = path.join(BASE_DIR, 'claude-browser-profile');
const LOCAL_STATE_PATH = path.join(PROFILE_DIR, 'Local State');
const COOKIES_DB_PATH = path.join(PROFILE_DIR, 'Default', 'Network', 'Cookies');
const USAGE_CACHE_PATH = path.join(BASE_DIR, 'claude-web-usage.json');
const STATUS_PATH = path.join(BASE_DIR, 'claude-web-helper-status.json');
const WATCH_LOCK_PATH = path.join(BASE_DIR, 'claude-web-helper-watch.lock');
const CONFIG_PATH = path.join(BASE_DIR, 'helper-config.json');
const DEFAULT_REFRESH_MINUTES = 5;
const MIN_REFRESH_MS = 60 * 1000;
const MAX_BACKOFF_MS = 60 * 60 * 1000;
const ACTIVE_MIN_INTERVAL_MS = 60 * 1000;
const ACTIVITY_SETTLE_MS = 5 * 1000;
const ACTIVITY_REFRESH_STATES = new Set(['ok', 'request_failed']);
const DEFAULT_REFRESH_MS = resolveRefreshMs();
const USER_AGENT =
  'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36';

let cachedMasterKey = null;
let watchLockHandle = null;

// Refresh interval: CLAUDE_WEB_HELPER_REFRESH_MS, else claude_refresh_minutes in helper-config.json,
// else 5 minutes. Never faster than once a minute.
function resolveRefreshMs() {
  const fromEnv = Number(process.env.CLAUDE_WEB_HELPER_REFRESH_MS);
  if (Number.isFinite(fromEnv) && fromEnv > 0) {
    return Math.max(MIN_REFRESH_MS, Math.round(fromEnv));
  }
  let minutes = DEFAULT_REFRESH_MINUTES;
  try {
    const config = JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8').replace(/^\uFEFF/, ''));
    const configured = Number(config && config.claude_refresh_minutes);
    if (Number.isFinite(configured) && configured > 0) {
      minutes = configured;
    }
  } catch {
    // no config file: use the default
  }
  return Math.max(MIN_REFRESH_MS, Math.round(minutes * 60 * 1000));
}

// Retry-After is either delta-seconds or an HTTP date. Returns an absolute epoch ms or null.
export function parseRetryAfter(value, nowMs) {
  if (value === null || value === undefined) {
    return null;
  }
  const text = String(value).trim();
  if (/^\d+(\.\d+)?$/.test(text)) {
    return nowMs + Math.ceil(Number(text) * 1000);
  }
  const dateMs = Date.parse(text);
  return Number.isFinite(dateMs) ? Math.max(nowMs, dateMs) : null;
}

// Delay before the next watch refresh. Rate limits wait for Retry-After (or back off
// exponentially from the refresh interval when the header is missing).
export function computeNextDelayMs({ state, refreshMs, consecutiveRateLimits, retryAfterAtMs, nowMs }) {
  if (state !== 'rate_limited') {
    return refreshMs;
  }
  const backoff = Math.min(MAX_BACKOFF_MS, refreshMs * 2 ** Math.max(0, consecutiveRateLimits));
  const retryAfter = retryAfterAtMs ? Math.max(0, retryAfterAtMs - nowMs) : 0;
  return Math.max(refreshMs, retryAfter || backoff);
}

// When the next watch fetch is due, measured from when the last fetch finished. Claude Code activity
// pulls it forward to ACTIVITY_SETTLE_MS after the activity, but never sooner than
// ACTIVE_MIN_INTERVAL_MS after the last fetch. Rate limits and sign-in problems keep their schedule.
export function computeNextFetchAtMs({
  lastFetchAtMs,
  pendingActivitySinceMs,
  state,
  refreshMs,
  consecutiveRateLimits,
  retryAfterAtMs,
}) {
  const scheduledAt =
    lastFetchAtMs +
    computeNextDelayMs({
      state,
      refreshMs,
      consecutiveRateLimits: consecutiveRateLimits - 1,
      retryAfterAtMs,
      nowMs: lastFetchAtMs,
    });
  if (pendingActivitySinceMs === null || !ACTIVITY_REFRESH_STATES.has(state)) {
    return scheduledAt;
  }
  const activityAt = Math.max(lastFetchAtMs + ACTIVE_MIN_INTERVAL_MS, pendingActivitySinceMs + ACTIVITY_SETTLE_MS);
  return Math.min(scheduledAt, activityAt);
}

// Watch-mode schedule on a monotonic clock, so a system clock change never stalls or rushes a fetch.
// Retry-After arrives as a wall-clock time and is converted once, when the fetch finishes.
export function createWatchSchedule({ refreshMs, now = () => performance.now(), wallNow = () => Date.now() }) {
  let lastFetchAtMs = null;
  let pendingActivitySinceMs = null;
  let result = { lastState: null, consecutiveRateLimits: 0, retryAfterAtMs: null };

  return {
    // Returns true when this is the first activity since the last fetch started.
    noteActivity() {
      if (pendingActivitySinceMs !== null) {
        return false;
      }
      pendingActivitySinceMs = now();
      return true;
    },
    // Activity before a fetch is covered by it; activity during the fetch schedules the next one.
    fetchStarted() {
      pendingActivitySinceMs = null;
    },
    fetchCompleted({ lastState, consecutiveRateLimits, retryAfterAtMs }) {
      lastFetchAtMs = now();
      result = {
        lastState,
        consecutiveRateLimits,
        retryAfterAtMs: retryAfterAtMs ? lastFetchAtMs + (retryAfterAtMs - wallNow()) : null,
      };
    },
    msUntilNextFetch() {
      if (lastFetchAtMs === null) {
        return 0;
      }
      const nextAtMs = computeNextFetchAtMs({
        lastFetchAtMs,
        pendingActivitySinceMs,
        state: result.lastState,
        refreshMs,
        consecutiveRateLimits: result.consecutiveRateLimits,
        retryAfterAtMs: result.retryAfterAtMs,
      });
      return nextAtMs - now();
    },
  };
}

// Claude Code writes its session transcripts to <config dir>\projects\<project>\*.jsonl.
export function resolveClaudeProjectsDir(env = process.env) {
  const configDir = String(env.CLAUDE_CONFIG_DIR || '').trim() || path.join(env.USERPROFILE || os.homedir(), '.claude');
  return path.join(configDir, 'projects');
}

// Calls onActivity whenever a Claude Code transcript changes. A missing folder or a watcher error
// is retried every retryMs, so a later Claude Code install is picked up without a restart.
export function watchClaudeActivity(dir, onActivity, { retryMs = 60 * 1000 } = {}) {
  let watcher = null;
  let retryTimer = null;
  let closed = false;

  const scheduleRetry = () => {
    if (closed) {
      return;
    }
    retryTimer = setTimeout(start, retryMs);
    retryTimer.unref();
  };

  function start() {
    if (closed) {
      return;
    }
    try {
      watcher = fs.watch(dir, { recursive: true }, (_event, filename) => {
        // A missing file name means the change buffer overflowed; count it as activity.
        if (filename && !String(filename).toLowerCase().endsWith('.jsonl')) {
          return;
        }
        onActivity();
      });
      // The watch loop's own timers keep the helper running; the watcher must not keep a failed one alive.
      watcher.unref();
      watcher.on('error', () => {
        watcher.close();
        watcher = null;
        scheduleRetry();
      });
    } catch {
      watcher = null;
      scheduleRetry();
    }
  }

  start();
  return {
    close() {
      closed = true;
      clearTimeout(retryTimer);
      if (watcher) {
        watcher.close();
      }
      watcher = null;
    },
  };
}

function getBrowserPath() {
  const override = process.env.CLAUDE_WEB_HELPER_BROWSER;
  if (override && fs.existsSync(override)) {
    return override;
  }

  const candidates = [
    'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
  ];

  return candidates.find((candidate) => fs.existsSync(candidate)) || null;
}

async function ensureBaseDir() {
  await fsp.mkdir(BASE_DIR, { recursive: true });
}

async function ensureProfileDir() {
  await fsp.mkdir(PROFILE_DIR, { recursive: true });
}

async function atomicWriteJson(filePath, value) {
  const tempPath = `${filePath}.tmp`;
  const text = `${JSON.stringify(value, null, 2)}\n`;
  await fsp.writeFile(tempPath, text, 'utf8');
  await fsp.rename(tempPath, filePath);
}

async function removeFileIfExists(filePath) {
  try {
    await fsp.unlink(filePath);
  } catch (error) {
    if (error && error.code !== 'ENOENT') {
      throw error;
    }
  }
}

async function writeStatus(state, details = {}) {
  await ensureBaseDir();
  await atomicWriteJson(STATUS_PATH, {
    state,
    updated_at: new Date().toISOString(),
    ...details,
  });
}

function matchesHelperWatchProcess(processInfo, watchLock) {
  if (!processInfo || !watchLock) {
    return false;
  }
  if (String(watchLock.mode || '').toLowerCase() !== 'watch') {
    return false;
  }

  const name = String(processInfo.name || '').toLowerCase();
  const commandLine = String(processInfo.command_line || '');
  if (name !== 'node.exe' || !/index\.mjs/i.test(commandLine) || !/(^|\s)watch(\s|$)/i.test(commandLine)) {
    return false;
  }

  const processStartedAt = Date.parse(String(processInfo.created_at || ''));
  const lockStartedAt = Date.parse(String(watchLock.started_at || ''));
  if (!Number.isFinite(processStartedAt) || !Number.isFinite(lockStartedAt)) {
    return false;
  }

  return Math.abs(lockStartedAt - processStartedAt) <= 30_000;
}

function getWindowsProcessInfo(pid) {
  const script = [
    `$process = Get-CimInstance Win32_Process -Filter "ProcessId = ${pid}" -ErrorAction SilentlyContinue`,
    'if ($null -eq $process) { exit 3 }',
    '[pscustomobject]@{',
    '  name = [string]$process.Name',
    '  command_line = [string]$process.CommandLine',
    "  created_at = ([DateTimeOffset]$process.CreationDate).ToUniversalTime().ToString('o')",
    '} | ConvertTo-Json -Compress',
  ].join('\n');

  return JSON.parse(runPowerShell(script));
}

function isHelperWatchRunning(watchLock) {
  const pid = Number(watchLock && watchLock.pid);
  if (!Number.isInteger(pid) || pid <= 0) {
    return false;
  }

  if (process.platform !== 'win32') {
    return false;
  }

  try {
    return matchesHelperWatchProcess(getWindowsProcessInfo(pid), watchLock);
  } catch {
    return false;
  }
}

async function tryReadJsonFile(filePath) {
  try {
    return JSON.parse(await fsp.readFile(filePath, 'utf8'));
  } catch (error) {
    if (error && error.code === 'ENOENT') {
      return null;
    }
    return null;
  }
}

function releaseWatchLock() {
  if (!watchLockHandle) {
    return;
  }

  try {
    if (typeof watchLockHandle.fd === 'number') {
      fs.closeSync(watchLockHandle.fd);
    }
  } catch {
    // ignore close failure during shutdown
  }

  try {
    fs.unlinkSync(WATCH_LOCK_PATH);
  } catch (error) {
    if (!error || error.code !== 'ENOENT') {
      // ignore cleanup failure during shutdown
    }
  }

  watchLockHandle = null;
}

async function acquireWatchLock() {
  await ensureBaseDir();

  for (let attempt = 0; attempt < 2; attempt += 1) {
    try {
      const handle = await fsp.open(WATCH_LOCK_PATH, 'wx');
      const payload = {
        pid: process.pid,
        mode: 'watch',
        started_at: new Date().toISOString(),
        refresh_ms: DEFAULT_REFRESH_MS,
      };
      await handle.writeFile(`${JSON.stringify(payload, null, 2)}\n`, 'utf8');
      watchLockHandle = handle;
      return true;
    } catch (error) {
      if (!error || error.code !== 'EEXIST') {
        throw error;
      }

      const existingLock = await tryReadJsonFile(WATCH_LOCK_PATH);
      const existingPid = Number(existingLock && existingLock.pid);
      if (isHelperWatchRunning(existingLock) && existingPid !== process.pid) {
        console.log(`Claude helper watch already running (pid ${existingPid}).`);
        return false;
      }

      await removeFileIfExists(WATCH_LOCK_PATH);
    }
  }

  throw new Error('Claude helper watch lock could not be acquired');
}

function isProbablyJson(text) {
  const trimmed = String(text || '').trim();
  return trimmed.startsWith('{') || trimmed.startsWith('[');
}

function classifyBody(text) {
  const body = String(text || '');
  if (!body) {
    return 'empty';
  }
  if (body.includes('Just a moment')) {
    return 'cloudflare_blocked';
  }
  if (body.includes('Enable JavaScript and cookies to continue')) {
    return 'cloudflare_challenge';
  }
  if (body.includes('<html')) {
    return 'unexpected_html';
  }
  return 'unknown';
}

// cedar_ember=1 adds the usage-limit reset grants (read only); other fields stay as without it.
export function buildUsageUrl(organizationId) {
  return `https://claude.ai/api/organizations/${organizationId}/usage?cedar_ember=1`;
}

// Usage-limit reset grants -> { available_count, earliest_expires_at } counted like the claude.ai
// settings page: unpaused grants, resets_left each (a usable grant with 0 left counts once).
// Returns null when the account is not eligible or the field is missing.
export function summarizeResetGrants(cedarEmber) {
  if (!cedarEmber || typeof cedarEmber !== 'object' || cedarEmber.eligible !== true) {
    return null;
  }
  let count = 0;
  let earliest = null;
  for (const grant of Array.isArray(cedarEmber.grants) ? cedarEmber.grants : []) {
    if (!grant || grant.paused === true) {
      continue;
    }
    const left = Number.isFinite(grant.resets_left) && grant.resets_left > 0 ? Math.trunc(grant.resets_left) : grant.usable_now === true ? 1 : 0;
    if (left === 0) {
      continue;
    }
    count += left;
    const endsAtMs = Date.parse(grant.ends_at);
    if (Number.isFinite(endsAtMs)) {
      const endsAt = Math.floor(endsAtMs / 1000);
      earliest = earliest === null ? endsAt : Math.min(earliest, endsAt);
    }
  }
  return { available_count: count, earliest_expires_at: earliest };
}

function normalizeUsagePayload(raw) {
  if (!raw || typeof raw !== 'object') {
    throw new Error('Invalid usage payload');
  }

  if (!raw.five_hour && !raw.seven_day) {
    throw new Error('Usage payload missing five_hour/seven_day');
  }

  return {
    source: 'claude-web-helper',
    generated_at: new Date().toISOString(),
    five_hour: raw.five_hour || null,
    seven_day: raw.seven_day || null,
    seven_day_sonnet: raw.seven_day_sonnet || null,
    extra_usage: raw.extra_usage || null,
    reset_credits: summarizeResetGrants(raw.cedar_ember),
  };
}

function getLocalStatePayload() {
  if (!fs.existsSync(LOCAL_STATE_PATH)) {
    throw new Error(`Claude helper local state not found at ${LOCAL_STATE_PATH}`);
  }

  return JSON.parse(fs.readFileSync(LOCAL_STATE_PATH, 'utf8'));
}

function runPowerShell(script) {
  const encodedCommand = Buffer.from(script, 'utf16le').toString('base64');
  const candidates = ['pwsh', 'powershell.exe'];
  const failures = [];

  for (const executable of candidates) {
    const result = spawnSync(executable, ['-NoProfile', '-EncodedCommand', encodedCommand], {
      encoding: 'utf8',
      windowsHide: true,
    });
    if (!result.error && result.status === 0) {
      return result.stdout.trim();
    }

    failures.push(
      `${executable}: ${
        result.error ? result.error.message : `exited ${result.status}: ${(result.stderr || '').trim()}`
      }`,
    );
  }

  throw new Error(
    `Failed to run PowerShell for Chromium DPAPI decryption (${failures.join('; ')})`,
  );
}

function getMasterKey() {
  if (cachedMasterKey) {
    return cachedMasterKey;
  }

  const localState = getLocalStatePayload();
  const encryptedKeyBase64 = localState?.os_crypt?.encrypted_key;
  if (!encryptedKeyBase64) {
    throw new Error('Chromium master key missing from Local State');
  }

  const encryptedKey = Buffer.from(encryptedKeyBase64, 'base64');
  const prefix = encryptedKey.subarray(0, 5).toString('utf8');
  if (prefix !== 'DPAPI') {
    throw new Error(`Unsupported Chromium key prefix: ${prefix || 'unknown'}`);
  }

  const dpapiPayload = encryptedKey.subarray(5);
  const pwshScript =
    `if (-not ('System.Security.Cryptography.ProtectedData' -as [type])) { ` +
    `Add-Type -AssemblyName System.Security }; ` +
    `[Convert]::ToBase64String(` +
    `[System.Security.Cryptography.ProtectedData]::Unprotect(` +
    `[Convert]::FromBase64String('${dpapiPayload.toString('base64')}'), ` +
    `$null, ` +
    `[System.Security.Cryptography.DataProtectionScope]::CurrentUser))`;
  cachedMasterKey = Buffer.from(runPowerShell(pwshScript), 'base64');
  if (!cachedMasterKey.length) {
    throw new Error('Chromium master key was empty');
  }

  return cachedMasterKey;
}

function copyCookiesDbForRead() {
  if (!fs.existsSync(COOKIES_DB_PATH)) {
    throw new Error(`Claude helper cookies DB not found at ${COOKIES_DB_PATH}`);
  }

  const tempPath = path.join(os.tmpdir(), `tm-claude-cookies-${process.pid}-${Date.now()}.sqlite`);
  try {
    fs.copyFileSync(COOKIES_DB_PATH, tempPath);
  } catch (error) {
    if (error && (error.code === 'EPERM' || error.code === 'EBUSY')) {
      throw new Error('Claude helper browser profile is still in use. Close the helper browser window and retry.');
    }
    throw error;
  }

  return tempPath;
}

function decryptCookieValue(encryptedValue, hostKey, masterKey) {
  if (!encryptedValue || !encryptedValue.length) {
    return '';
  }

  const payload = Buffer.from(encryptedValue);
  const version = payload.subarray(0, 3).toString('utf8');
  if (version !== 'v10' && version !== 'v11') {
    throw new Error(`Unsupported Chromium cookie version: ${version || 'unknown'}`);
  }

  const nonce = payload.subarray(3, 15);
  const ciphertext = payload.subarray(15, payload.length - 16);
  const authTag = payload.subarray(payload.length - 16);
  const decipher = crypto.createDecipheriv('aes-256-gcm', masterKey, nonce);
  decipher.setAuthTag(authTag);

  let plaintext = Buffer.concat([decipher.update(ciphertext), decipher.final()]);
  const hostHash = crypto.createHash('sha256').update(hostKey).digest();
  if (plaintext.length >= 32 && plaintext.subarray(0, 32).equals(hostHash)) {
    plaintext = plaintext.subarray(32);
  }

  return plaintext.toString('utf8');
}

function loadClaudeCookies() {
  const masterKey = getMasterKey();
  const tempDbPath = copyCookiesDbForRead();

  try {
    const database = new DatabaseSync(tempDbPath, { readonly: true });
    try {
      const rows = database
        .prepare(
          `select host_key, name, value, encrypted_value
           from cookies
           where host_key like '%claude.ai%'
           order by case when host_key = '.claude.ai' then 0 else 1 end, name`,
        )
        .all();

      const cookieMap = new Map();
      for (const row of rows) {
        if (cookieMap.has(row.name)) {
          continue;
        }

        let value = row.value || '';
        if (!value) {
          value = decryptCookieValue(row.encrypted_value, row.host_key, masterKey);
        }

        if (value) {
          cookieMap.set(row.name, value);
        }
      }

      if (!cookieMap.has('sessionKey')) {
        throw new Error('Claude helper session cookie not found. Run login again.');
      }

      return cookieMap;
    } finally {
      database.close();
    }
  } finally {
    try {
      fs.unlinkSync(tempDbPath);
    } catch {
      // ignore temp cleanup failure
    }
  }
}

function buildCookieHeader(cookieMap) {
  return Array.from(cookieMap.entries())
    .map(([name, value]) => `${name}=${value}`)
    .join('; ');
}

async function fetchJsonWithCookies(url, cookieHeader) {
  const response = await fetch(url, {
    headers: {
      accept: 'application/json',
      cookie: cookieHeader,
      origin: 'https://claude.ai',
      referer: 'https://claude.ai/',
      'user-agent': USER_AGENT,
    },
  });

  const text = await response.text();
  if (!response.ok) {
    const error = new Error(`HTTP ${response.status}`);
    error.code = 'HTTP_ERROR';
    error.httpStatus = response.status;
    error.retryAfterAtMs = parseRetryAfter(response.headers.get('retry-after'), Date.now());
    error.body = text;
    throw error;
  }

  if (!isProbablyJson(text)) {
    const error = new Error(`Non-JSON response: ${classifyBody(text)}`);
    error.code = 'NON_JSON';
    error.body = text;
    throw error;
  }

  return JSON.parse(text);
}

function pickClaudeOrganization(organizations, lastActiveOrg) {
  if (lastActiveOrg) {
    const activeOrganization = organizations.find(
      (organization) => organization?.uuid === lastActiveOrg || String(organization?.id) === String(lastActiveOrg),
    );
    if (activeOrganization) {
      return activeOrganization;
    }
  }

  return (
    organizations.find((organization) => Array.isArray(organization?.capabilities) && organization.capabilities.includes('chat')) ||
    organizations.find((organization) => typeof organization?.rate_limit_tier === 'string' && organization.rate_limit_tier.includes('claude')) ||
    organizations[0]
  );
}

function shouldRetryWithOrganizationLookup(error) {
  return Boolean(error && error.httpStatus && (error.httpStatus === 400 || error.httpStatus === 404));
}

async function fetchUsageForOrganization(cookieHeader, organizationId, organizationName = null) {
  if (!organizationId) {
    throw new Error('Organization id not found');
  }

  const usage = await fetchJsonWithCookies(buildUsageUrl(organizationId), cookieHeader);
  const payload = normalizeUsagePayload(usage);
  return { organizationId, organizationName, payload };
}

async function fetchUsageSnapshot() {
  const cookies = loadClaudeCookies();
  const cookieHeader = buildCookieHeader(cookies);
  const lastActiveOrg = cookies.get('lastActiveOrg');
  if (lastActiveOrg) {
    try {
      return await fetchUsageForOrganization(cookieHeader, lastActiveOrg);
    } catch (error) {
      if (!shouldRetryWithOrganizationLookup(error)) {
        throw error;
      }
    }
  }

  const organizations = await fetchJsonWithCookies('https://claude.ai/api/organizations', cookieHeader);
  if (!Array.isArray(organizations) || organizations.length === 0) {
    throw new Error('No Claude organizations available');
  }

  const organization = pickClaudeOrganization(organizations, lastActiveOrg);
  const organizationId = organization?.uuid || organization?.id;
  return fetchUsageForOrganization(cookieHeader, organizationId, organization?.name || null);
}

function classifyError(error) {
  const message = error && error.message ? error.message : String(error);
  if (error && error.httpStatus === 401) {
    return 'login_required';
  }
  if (error && error.httpStatus === 403) {
    return 'access_denied';
  }
  if (error && error.httpStatus === 429) {
    return 'rate_limited';
  }
  if (message.includes('browser profile is still in use')) {
    return 'profile_in_use';
  }
  if (message.includes('session cookie not found')) {
    return 'login_required';
  }
  if (message.includes('cloudflare_blocked') || message.includes('cloudflare_challenge')) {
    return 'cloudflare_blocked';
  }
  if (message.includes('Non-JSON response')) {
    return 'login_required';
  }
  return 'request_failed';
}

function shouldRetainUsageSnapshotOnFailure(state) {
  return state === 'request_failed' || state === 'rate_limited';
}

async function writeUsagePayload(payload, organizationId, organizationName) {
  await ensureBaseDir();
  await atomicWriteJson(USAGE_CACHE_PATH, { ...payload, refresh_ms: DEFAULT_REFRESH_MS });
  await writeStatus('ok', {
    organization_id: organizationId,
    organization_name: organizationName,
    usage_path: USAGE_CACHE_PATH,
    refresh_ms: DEFAULT_REFRESH_MS,
  });
}

async function runOnce(watchState = null) {
  try {
    const { organizationId, organizationName, payload } = await fetchUsageSnapshot();
    await writeUsagePayload(payload, organizationId, organizationName);
    console.log(`Claude helper updated ${USAGE_CACHE_PATH}`);
    if (watchState) {
      watchState.lastState = 'ok';
      watchState.consecutiveRateLimits = 0;
      watchState.retryAfterAtMs = null;
    }
    return 0;
  } catch (error) {
    await ensureBaseDir();
    const state = classifyError(error);
    const retainedUsageSnapshot = shouldRetainUsageSnapshotOnFailure(state) && fs.existsSync(USAGE_CACHE_PATH);
    if (!retainedUsageSnapshot) {
      await removeFileIfExists(USAGE_CACHE_PATH);
    }
    const retryAfterAtMs = error && error.retryAfterAtMs ? error.retryAfterAtMs : null;
    if (watchState) {
      watchState.lastState = state;
      watchState.retryAfterAtMs = retryAfterAtMs;
      watchState.consecutiveRateLimits = state === 'rate_limited' ? watchState.consecutiveRateLimits + 1 : 0;
    }
    await writeStatus(state, {
      error: error && error.message ? error.message : String(error),
      retained_usage_snapshot: retainedUsageSnapshot,
      usage_path: retainedUsageSnapshot ? USAGE_CACHE_PATH : undefined,
      retry_after_at: retryAfterAtMs ? new Date(retryAfterAtMs).toISOString() : undefined,
      refresh_ms: DEFAULT_REFRESH_MS,
    });
    console.error(
      `Claude helper failed: ${error && error.message ? error.message : error}${
        retainedUsageSnapshot ? ' (keeping recent usage snapshot)' : ''
      }`,
    );
    return 1;
  }
}

async function runLogin() {
  await ensureBaseDir();
  await ensureProfileDir();

  const executablePath = getBrowserPath();
  if (!executablePath) {
    throw new Error('Browser executable not found. Set CLAUDE_WEB_HELPER_BROWSER.');
  }

  spawn(executablePath, [`--user-data-dir=${PROFILE_DIR}`, '--new-window', 'https://claude.ai/login'], {
    detached: true,
    stdio: 'ignore',
  }).unref();

  await writeStatus('login_browser_opened', {
    profile_dir: PROFILE_DIR,
  });

  console.log('Opened a normal browser window for Claude login.');
  console.log('Complete the login there, then close that helper browser window before running once/watch.');
  return 0;
}

async function runWatch() {
  if (!(await acquireWatchLock())) {
    return 0;
  }

  const releaseAndExit = (exitCode) => {
    releaseWatchLock();
    process.exit(exitCode);
  };

  process.once('SIGINT', () => releaseAndExit(0));
  process.once('SIGTERM', () => releaseAndExit(0));
  process.once('exit', releaseWatchLock);

  const watchState = { lastState: null, consecutiveRateLimits: 0, retryAfterAtMs: null };
  const schedule = createWatchSchedule({ refreshMs: DEFAULT_REFRESH_MS });
  let wakeUp = null;
  watchClaudeActivity(resolveClaudeProjectsDir(), () => {
    if (schedule.noteActivity() && wakeUp) {
      wakeUp();
    }
  });

  while (true) {
    schedule.fetchStarted();
    await runOnce(watchState);
    schedule.fetchCompleted(watchState);

    if (watchState.lastState === 'rate_limited') {
      console.log(`Claude helper rate limited; next request in ${Math.round(schedule.msUntilNextFetch() / 1000)}s`);
    }
    for (let waitMs = schedule.msUntilNextFetch(); waitMs > 0; waitMs = schedule.msUntilNextFetch()) {
      // Wait in bounded steps; an activity wake-up ends the wait early.
      await new Promise((resolve) => {
        const timer = setTimeout(resolve, Math.min(waitMs, 60 * 1000));
        wakeUp = () => {
          clearTimeout(timer);
          resolve();
        };
      });
      wakeUp = null;
    }
  }
}

async function main() {
  switch (MODE) {
    case 'login':
      process.exitCode = await runLogin();
      break;
    case 'once':
      process.exitCode = await runOnce();
      break;
    case 'watch':
      await runWatch();
      break;
    default:
      console.error(`Unknown mode: ${MODE}`);
      console.error('Use one of: login, once, watch');
      process.exitCode = 1;
      break;
  }
}

// Run only when executed as a script, so tests can import the pure helpers without side effects.
const invokedDirectly =
  Boolean(process.argv[1]) && path.resolve(process.argv[1]).toLowerCase() === fileURLToPath(import.meta.url).toLowerCase();

if (invokedDirectly) {
  main().catch(async (error) => {
    try {
      await ensureBaseDir();
      await writeStatus('crashed', {
        error: error && error.message ? error.message : String(error),
      });
    } catch {
      // ignore secondary failure
    }
    console.error(error);
    process.exitCode = 1;
  });
}
