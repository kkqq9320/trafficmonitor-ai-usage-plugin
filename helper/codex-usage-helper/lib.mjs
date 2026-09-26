// Pure logic for the Codex usage helper. No I/O lives here so it can be unit tested.
//
// Data sources, in priority order:
//   1. codex app-server "account/rateLimits/read" (server value; Codex refreshes auth itself)
//   2. GET https://chatgpt.com/backend-api/wham/usage (server value; reads ~/.codex/auth.json)
//   3. Session JSONL token_count events (local value written by Codex after each model turn)
// None of these sources sends a model request or spends model tokens.

export const CODEX_LIMIT_ID = 'codex';
export const FIVE_HOUR_MINUTES = 300;
export const SEVEN_DAY_MINUTES = 10080;
const WINDOW_TOLERANCE_MINUTES = 1;

// The only JSON-RPC requests and HTTP endpoints the helper is allowed to use.
export const ALLOWED_APP_SERVER_REQUESTS = Object.freeze(['initialize', 'account/rateLimits/read']);
export const ALLOWED_APP_SERVER_NOTIFICATIONS = Object.freeze(['initialized']);
export const WHAM_USAGE_URL = 'https://chatgpt.com/backend-api/wham/usage';
export const ALLOWED_HTTP_URLS = Object.freeze([WHAM_USAGE_URL]);

export const DEFAULTS = Object.freeze({
  serverRefreshMs: 15 * 60 * 1000,
  minServerRefreshMs: 30 * 1000,
  minServerSpacingMs: 5 * 60 * 1000,
  maxFailureBackoffMs: 60 * 60 * 1000,
  defaultRateLimitBackoffMs: 15 * 60 * 1000,
  resetGraceMs: 30 * 1000,
});

export function assertAllowedAppServerRequest(method) {
  if (!ALLOWED_APP_SERVER_REQUESTS.includes(method)) {
    throw new Error('Blocked app-server request: ' + method);
  }
}

export function assertAllowedAppServerNotification(method) {
  if (!ALLOWED_APP_SERVER_NOTIFICATIONS.includes(method)) {
    throw new Error('Blocked app-server notification: ' + method);
  }
}

export function assertAllowedHttpUrl(url) {
  if (!ALLOWED_HTTP_URLS.includes(String(url))) {
    throw new Error('Blocked HTTP request: ' + url);
  }
}

function isFiniteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

export function classifyWindowMinutes(minutes) {
  if (!isFiniteNumber(minutes)) return null;
  if (Math.abs(minutes - FIVE_HOUR_MINUTES) <= WINDOW_TOLERANCE_MINUTES) return 'five_hour';
  if (Math.abs(minutes - SEVEN_DAY_MINUTES) <= WINDOW_TOLERANCE_MINUTES) return 'seven_day';
  return null;
}

export function makeWindow(usedPercent, windowMinutes, resetsAt) {
  if (!isFiniteNumber(usedPercent)) return null;
  return {
    used_percent: Math.min(100, Math.max(0, usedPercent)),
    window_minutes: isFiniteNumber(windowMinutes) && windowMinutes > 0 ? Math.round(windowMinutes) : null,
    resets_at: isFiniteNumber(resetsAt) && resetsAt > 0 ? Math.trunc(resetsAt) : null,
  };
}

// Classifies by window length, not by primary/secondary position. Unknown lengths are dropped.
// Legacy payloads without window_minutes fall back to primary = 5h, secondary = 7d.
export function assignWindows(primary, secondary) {
  const result = { five_hour: null, seven_day: null };
  for (const window of [primary, secondary]) {
    if (!window || window.window_minutes === null) continue;
    const kind = classifyWindowMinutes(window.window_minutes);
    if (kind && !result[kind]) {
      result[kind] = { ...window, window_minutes: kind === 'five_hour' ? FIVE_HOUR_MINUTES : SEVEN_DAY_MINUTES };
    }
  }
  if (primary && primary.window_minutes === null && !result.five_hour) {
    result.five_hour = { ...primary, window_minutes: FIVE_HOUR_MINUTES };
  }
  if (secondary && secondary.window_minutes === null && !result.seven_day) {
    result.seven_day = { ...secondary, window_minutes: SEVEN_DAY_MINUTES };
  }
  return result;
}

export function normalizeReachedType(value) {
  if (typeof value === 'string' && value) return value;
  if (value && typeof value === 'object' && typeof value.type === 'string' && value.type) return value.type;
  return null;
}

// Free rate limit resets the account holds ("Full reset" credits). Returns
// { available_count, earliest_expires_at } or null when the server does not report them.
function toResetCredits(availableCount, credits) {
  if (!isFiniteNumber(availableCount)) return null;
  const expiries = (Array.isArray(credits) ? credits : [])
    .filter((credit) => credit && credit.status === 'available' && isFiniteNumber(credit.expiresAt))
    .map((credit) => Math.trunc(credit.expiresAt));
  return {
    available_count: Math.max(0, Math.trunc(availableCount)),
    earliest_expires_at: expiries.length ? Math.min(...expiries) : null,
  };
}

// account/rateLimits/read result -> normalized record. Only the "codex" limit bucket is used.
export function normalizeAppServerRateLimits(result) {
  if (!result || typeof result !== 'object') throw new Error('Empty app-server rate limit result');
  let bucket = null;
  const byId = result.rateLimitsByLimitId;
  if (byId && typeof byId === 'object' && byId[CODEX_LIMIT_ID]) {
    bucket = byId[CODEX_LIMIT_ID];
  } else if (result.rateLimits && (!result.rateLimits.limitId || result.rateLimits.limitId === CODEX_LIMIT_ID)) {
    bucket = result.rateLimits;
  }
  if (!bucket) throw new Error('app-server result has no "codex" rate limit bucket');

  const toWindow = (raw) => (raw ? makeWindow(raw.usedPercent, raw.windowDurationMins, raw.resetsAt) : null);
  const windows = assignWindows(toWindow(bucket.primary), toWindow(bucket.secondary));
  const resetCredits = result.rateLimitResetCredits;
  return {
    ...windows,
    plan_type: typeof bucket.planType === 'string' ? bucket.planType : null,
    rate_limit_reached_type: normalizeReachedType(bucket.rateLimitReachedType),
    reset_credits: resetCredits ? toResetCredits(resetCredits.availableCount, resetCredits.credits) : null,
  };
}

// GET wham/usage payload -> normalized record.
export function normalizeWhamUsage(payload) {
  if (!payload || typeof payload !== 'object' || typeof payload.plan_type !== 'string') {
    throw new Error('Unexpected wham/usage payload');
  }
  const rateLimit = payload.rate_limit || {};
  const toWindow = (raw) => {
    if (!raw) return null;
    const seconds = raw.limit_window_seconds;
    const minutes = isFiniteNumber(seconds) && seconds > 0 ? Math.ceil(seconds / 60) : null;
    return makeWindow(raw.used_percent, minutes, raw.reset_at);
  };
  const windows = assignWindows(toWindow(rateLimit.primary_window), toWindow(rateLimit.secondary_window));
  let reachedType = normalizeReachedType(payload.rate_limit_reached_type);
  if (!reachedType && rateLimit.limit_reached === true) reachedType = 'rate_limit_reached';
  const resetCredits = payload.rate_limit_reset_credits;
  return {
    ...windows,
    plan_type: payload.plan_type,
    rate_limit_reached_type: reachedType,
    reset_credits: resetCredits ? toResetCredits(resetCredits.available_count, null) : null,
  };
}

// Returns a normalized record for a Codex token_count event of the "codex" bucket that carries
// at least one real window, or null for anything else (other buckets, null windows, other events).
export function parseRateLimitEventLine(line) {
  if (typeof line !== 'string' || !line.includes('"token_count"') || !line.includes('"rate_limits"')) return null;
  let event;
  try {
    event = JSON.parse(line);
  } catch {
    return null;
  }
  if (!event || event.type !== 'event_msg' || !event.payload || event.payload.type !== 'token_count') return null;
  const limits = event.payload.rate_limits;
  if (!limits || typeof limits !== 'object') return null;
  if (limits.limit_id !== undefined && limits.limit_id !== null && limits.limit_id !== CODEX_LIMIT_ID) return null;

  const toWindow = (raw) => {
    if (!raw || typeof raw !== 'object') return null;
    let used = raw.used_percent;
    if (!isFiniteNumber(used) && isFiniteNumber(raw.remaining_percent)) used = 100 - raw.remaining_percent;
    const minutes = isFiniteNumber(raw.window_minutes) ? raw.window_minutes : null;
    const resetsAt = isFiniteNumber(raw.resets_at) ? raw.resets_at : raw.reset_at;
    return makeWindow(used, minutes, resetsAt);
  };
  const windows = assignWindows(toWindow(limits.primary), toWindow(limits.secondary));
  if (!windows.five_hour && !windows.seven_day) return null;

  const timestampMs = Date.parse(event.timestamp);
  if (!Number.isFinite(timestampMs)) return null;
  return {
    ...windows,
    plan_type: typeof limits.plan_type === 'string' ? limits.plan_type : null,
    rate_limit_reached_type: normalizeReachedType(limits.rate_limit_reached_type),
    timestampMs,
  };
}

// True for a JSONL event that reports Codex refusing a turn because the usage limit was hit.
export function isUsageLimitEventLine(line) {
  if (typeof line !== 'string' || !line.includes('usage_limit')) return false;
  let event;
  try {
    event = JSON.parse(line);
  } catch {
    return false;
  }
  if (!event || event.type !== 'event_msg' || !event.payload) return false;
  const info = event.payload.codex_error_info ?? (event.payload.error && event.payload.error.codex_error_info);
  const text = typeof info === 'string' ? info : info && typeof info === 'object' ? Object.keys(info).join(',') : '';
  return /usage_limit/i.test(text);
}

export function isLimitReached(record) {
  if (!record) return false;
  if (record.rate_limit_reached_type) return true;
  return [record.five_hour, record.seven_day].some((window) => window && window.used_percent >= 100);
}

export function buildSnapshot(record, { source, method, dataAtMs, fetchedAtMs, previous }) {
  const plan = record.plan_type || (previous && previous.plan_type) || null;
  return {
    schema: 1,
    limit_id: CODEX_LIMIT_ID,
    source,
    method,
    data_at: new Date(dataAtMs).toISOString(),
    data_at_unix: Math.floor(dataAtMs / 1000),
    fetched_at: new Date(fetchedAtMs).toISOString(),
    fetched_at_unix: Math.floor(fetchedAtMs / 1000),
    plan_type: plan,
    rate_limit_reached_type: record.rate_limit_reached_type || null,
    limit_reached: isLimitReached(record),
    five_hour: record.five_hour || null,
    seven_day: record.seven_day || null,
    // Session events carry no reset credits (undefined): keep the last server value.
    reset_credits: record.reset_credits !== undefined ? record.reset_credits : (previous && previous.reset_credits) || null,
  };
}

export function snapshotDataAtMs(snapshot) {
  if (!snapshot) return null;
  if (isFiniteNumber(snapshot.data_at_unix)) return snapshot.data_at_unix * 1000;
  const parsed = Date.parse(snapshot.data_at);
  return Number.isFinite(parsed) ? parsed : null;
}

export function isNewerThanSnapshot(dataAtMs, snapshot) {
  const current = snapshotDataAtMs(snapshot);
  return current === null || dataAtMs > current;
}

// Retry-After is either delta-seconds or an HTTP date. Returns an absolute epoch ms or null.
export function parseRetryAfter(value, nowMs) {
  if (value === null || value === undefined) return null;
  const text = String(value).trim();
  if (!text) return null;
  if (/^\d+(\.\d+)?$/.test(text)) return nowMs + Math.ceil(Number(text) * 1000);
  const dateMs = Date.parse(text);
  return Number.isFinite(dateMs) ? Math.max(nowMs, dateMs) : null;
}

export function clampServerRefreshMs(value) {
  if (!isFiniteNumber(value) || value <= 0) return DEFAULTS.serverRefreshMs;
  return Math.max(DEFAULTS.minServerRefreshMs, Math.round(value));
}

// Decides when the next server request may happen.
//   - regular refresh every refreshMs after the last attempt; after failures the interval
//     doubles (starting at max(refreshMs, minSpacingMs)) up to maxBackoffMs
//   - extra requests for a requested trigger (limit reached) or shortly after a known resets_at
//     that no request has covered yet; these extra requests keep minSpacingMs from the last attempt
//   - never before retryAfterUntilMs (429 Retry-After)
export function computeNextServerFetchMs({
  nowMs,
  lastAttemptMs = null,
  consecutiveFailures = 0,
  retryAfterUntilMs = null,
  refreshMs = DEFAULTS.serverRefreshMs,
  minSpacingMs = DEFAULTS.minServerSpacingMs,
  maxBackoffMs = DEFAULTS.maxFailureBackoffMs,
  resetGraceMs = DEFAULTS.resetGraceMs,
  snapshot = null,
  requestedAtMs = null,
}) {
  let next;
  if (lastAttemptMs === null) {
    next = nowMs;
  } else if (consecutiveFailures > 0) {
    const base = Math.max(refreshMs, minSpacingMs);
    next = lastAttemptMs + Math.min(maxBackoffMs, base * 2 ** (consecutiveFailures - 1));
  } else {
    next = lastAttemptMs + refreshMs;
  }

  const triggers = [];
  if (requestedAtMs !== null) triggers.push(requestedAtMs);
  if (snapshot) {
    for (const window of [snapshot.five_hour, snapshot.seven_day]) {
      if (!window || !isFiniteNumber(window.resets_at)) continue;
      const resetMs = window.resets_at * 1000 + resetGraceMs;
      if (lastAttemptMs === null || resetMs > lastAttemptMs) triggers.push(resetMs);
    }
  }
  for (const trigger of triggers) {
    const allowed = lastAttemptMs === null ? trigger : Math.max(trigger, lastAttemptMs + minSpacingMs);
    if (allowed < next) next = allowed;
  }

  if (retryAfterUntilMs !== null) next = Math.max(next, retryAfterUntilMs);
  return Math.max(next, nowMs);
}

// Picks the codex executable: explicit path, then codex.exe on PATH, then the newest
// %LOCALAPPDATA%\OpenAI\Codex\bin\<hash>\codex.exe (hash folders change on app updates and
// some of them do not contain codex.exe).
export function resolveCodexExecutable({ configuredPath, pathDirs = [], appBinDirs = [], exists }) {
  if (configuredPath) {
    return exists(configuredPath) ? { path: configuredPath, origin: 'config' } : null;
  }
  for (const dir of pathDirs) {
    if (!dir) continue;
    const candidate = dir.replace(/[\\/]+$/, '') + '\\codex.exe';
    if (exists(candidate)) return { path: candidate, origin: 'PATH' };
  }
  const withExe = appBinDirs
    .map((entry) => ({ ...entry, exe: entry.dir.replace(/[\\/]+$/, '') + '\\codex.exe' }))
    .filter((entry) => exists(entry.exe))
    .sort((a, b) => b.exeMtimeMs - a.exeMtimeMs);
  return withExe.length ? { path: withExe[0].exe, origin: 'Codex app' } : null;
}

export function classifyServerError(error) {
  const status = error && error.httpStatus;
  const message = String((error && error.message) || error || '');
  if (status === 429 || /\b429\b|too many requests/i.test(message)) return 'rate_limited';
  if (status === 401 || status === 403 || /\b401\b|unauthorized|sign in again|not logged in/i.test(message)) return 'auth_required';
  return 'request_failed';
}
