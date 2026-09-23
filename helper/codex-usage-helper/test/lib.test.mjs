import test from 'node:test';
import assert from 'node:assert/strict';
import * as lib from '../lib.mjs';

const MIN = 60 * 1000;

function tokenCountLine(timestamp, rateLimits) {
  return JSON.stringify({ timestamp, type: 'event_msg', payload: { type: 'token_count', info: null, rate_limits: rateLimits } });
}

test('window classification tolerates one minute of drift and drops unknown lengths', () => {
  assert.equal(lib.classifyWindowMinutes(300), 'five_hour');
  assert.equal(lib.classifyWindowMinutes(299), 'five_hour');
  assert.equal(lib.classifyWindowMinutes(301), 'five_hour');
  assert.equal(lib.classifyWindowMinutes(10080), 'seven_day');
  assert.equal(lib.classifyWindowMinutes(10081), 'seven_day');
  assert.equal(lib.classifyWindowMinutes(1440), null);
  assert.equal(lib.classifyWindowMinutes(undefined), null);
});

test('app-server result (shape observed 2026-09-23) maps to a weekly-only, limit-reached record', () => {
  const codex = {
    limitId: 'codex',
    primary: { usedPercent: 100, windowDurationMins: 10080, resetsAt: 1790567129 },
    secondary: null,
    planType: 'pro',
    rateLimitReachedType: 'rate_limit_reached',
  };
  const record = lib.normalizeAppServerRateLimits({
    rateLimits: codex,
    rateLimitsByLimitId: { codex },
  });
  assert.equal(record.five_hour, null);
  assert.deepEqual(record.seven_day, { used_percent: 100, window_minutes: 10080, resets_at: 1790567129 });
  assert.equal(record.plan_type, 'pro');
  assert.equal(record.rate_limit_reached_type, 'rate_limit_reached');
  assert.equal(lib.isLimitReached(record), true);
});

test('app-server result uses the "codex" bucket even when another bucket is on top', () => {
  const record = lib.normalizeAppServerRateLimits({
    rateLimits: { limitId: 'codex_bengalfox', primary: { usedPercent: 12, windowDurationMins: 300, resetsAt: 1 }, secondary: null },
    rateLimitsByLimitId: {
      codex_bengalfox: { limitId: 'codex_bengalfox', primary: { usedPercent: 12, windowDurationMins: 300, resetsAt: 1 } },
      codex: { limitId: 'codex', primary: { usedPercent: 40, windowDurationMins: 10080, resetsAt: 2 }, secondary: null },
    },
  });
  assert.equal(record.five_hour, null);
  assert.equal(record.seven_day.used_percent, 40);
  assert.throws(() => lib.normalizeAppServerRateLimits({ rateLimits: { limitId: 'premium', primary: null } }));
});

test('wham/usage payload (shape observed 2026-09-23) matches the app-server record', () => {
  const record = lib.normalizeWhamUsage({
    plan_type: 'pro',
    rate_limit: {
      allowed: false,
      limit_reached: true,
      primary_window: { used_percent: 100, limit_window_seconds: 604800, reset_after_seconds: 425280, reset_at: 1790567129 },
      secondary_window: null,
    },
    rate_limit_reached_type: { type: 'rate_limit_reached', details: 'default' },
  });
  assert.equal(record.five_hour, null);
  assert.deepEqual(record.seven_day, { used_percent: 100, window_minutes: 10080, resets_at: 1790567129 });
  assert.equal(record.rate_limit_reached_type, 'rate_limit_reached');

  const both = lib.normalizeWhamUsage({
    plan_type: 'plus',
    rate_limit: {
      limit_reached: false,
      primary_window: { used_percent: 12.5, limit_window_seconds: 18000, reset_at: 10 },
      secondary_window: { used_percent: 34, limit_window_seconds: 604800, reset_at: 20 },
    },
    rate_limit_reached_type: null,
  });
  assert.equal(both.five_hour.used_percent, 12.5);
  assert.equal(both.seven_day.used_percent, 34);
  assert.equal(both.rate_limit_reached_type, null);
  assert.throws(() => lib.normalizeWhamUsage({ detail: 'Unauthorized' }));
});

test('session JSONL events: only the codex bucket with real windows counts', () => {
  const ts = '2026-09-22T06:24:56.539Z';
  const codex = lib.parseRateLimitEventLine(tokenCountLine(ts, {
    limit_id: 'codex', primary: { used_percent: 94, window_minutes: 10080, resets_at: 1790567129 }, secondary: null,
  }));
  assert.equal(codex.seven_day.used_percent, 94);
  assert.equal(codex.timestampMs, Date.parse(ts));

  assert.equal(lib.parseRateLimitEventLine(tokenCountLine(ts, {
    limit_id: 'premium', primary: null, secondary: null, credits: { has_credits: false },
  })), null);
  assert.equal(lib.parseRateLimitEventLine(tokenCountLine(ts, {
    limit_id: 'codex_bengalfox',
    primary: { used_percent: 12, window_minutes: 300, resets_at: 1 },
    secondary: { used_percent: 70, window_minutes: 10080, resets_at: 2 },
  })), null);
  // opencodex/Anthropic turns write the codex bucket with null windows
  assert.equal(lib.parseRateLimitEventLine(tokenCountLine(ts, {
    limit_id: 'codex', primary: null, secondary: null, plan_type: null,
  })), null);
  // legacy payload without limit_id and with remaining_percent
  const legacy = lib.parseRateLimitEventLine(tokenCountLine(ts, {
    primary: { remaining_percent: 70, window_minutes: 300, resets_at: 5 },
  }));
  assert.equal(legacy.five_hour.used_percent, 30);
  assert.equal(lib.parseRateLimitEventLine(JSON.stringify({ timestamp: ts, type: 'response_item', payload: { type: 'message', rate_limits: 'token_count' } })), null);
  assert.equal(lib.parseRateLimitEventLine('{"token_count" "rate_limits" broken'), null);
});

test('usage limit errors in session JSONL are detected', () => {
  const hit = JSON.stringify({
    timestamp: '2026-09-22T07:02:17.878Z',
    type: 'event_msg',
    payload: { type: 'task_complete', turn_id: 't', error: { message: 'You have hit your usage limit', codex_error_info: 'usage_limit_exceeded' } },
  });
  assert.equal(lib.isUsageLimitEventLine(hit), true);
  const text = JSON.stringify({ timestamp: 'x', type: 'event_msg', payload: { type: 'agent_message', message: 'usage_limit_exceeded is a string here' } });
  assert.equal(lib.isUsageLimitEventLine(text), false);
});

test('server schedule: regular interval, reset trigger, spacing, backoff, Retry-After', () => {
  const now = Date.parse('2026-09-23T06:00:00Z');
  const refreshMs = 15 * MIN;

  assert.equal(lib.computeNextServerFetchMs({ nowMs: now }), now, 'first run fetches immediately');
  assert.equal(lib.computeNextServerFetchMs({ nowMs: now, lastAttemptMs: now, refreshMs }), now + 15 * MIN);

  const snapshot = { seven_day: { used_percent: 100, resets_at: Math.floor((now + 2 * MIN) / 1000) }, five_hour: null };
  assert.equal(
    lib.computeNextServerFetchMs({ nowMs: now, lastAttemptMs: now - 1 * MIN, refreshMs, snapshot }),
    now + 4 * MIN,
    'reset trigger waits for the 5 minute spacing',
  );
  assert.equal(
    lib.computeNextServerFetchMs({ nowMs: now, lastAttemptMs: now - 10 * MIN, refreshMs, snapshot }),
    now + 2 * MIN + 30 * 1000,
    'reset trigger fires 30s after resets_at',
  );
  const afterReset = now + 3 * MIN;
  assert.equal(
    lib.computeNextServerFetchMs({ nowMs: afterReset, lastAttemptMs: afterReset, consecutiveFailures: 1, refreshMs, snapshot }),
    afterReset + 15 * MIN,
    'a failed attempt after the reset falls back to backoff instead of retrying every 5 minutes',
  );

  assert.equal(lib.computeNextServerFetchMs({ nowMs: now, lastAttemptMs: now, consecutiveFailures: 2, refreshMs }), now + 30 * MIN);
  assert.equal(lib.computeNextServerFetchMs({ nowMs: now, lastAttemptMs: now, consecutiveFailures: 5, refreshMs }), now + 60 * MIN);

  assert.equal(
    lib.computeNextServerFetchMs({ nowMs: now, lastAttemptMs: now - 20 * MIN, refreshMs, retryAfterUntilMs: now + 50 * MIN }),
    now + 50 * MIN,
  );
  assert.equal(
    lib.computeNextServerFetchMs({ nowMs: now, lastAttemptMs: now - 2 * MIN, refreshMs, requestedAtMs: now }),
    now + 3 * MIN,
    'limit-reached trigger keeps the 5 minute spacing',
  );
  assert.equal(lib.clampServerRefreshMs(10 * 1000), 30 * 1000, 'refresh interval has a 30 second floor');
  assert.equal(lib.clampServerRefreshMs(NaN), 15 * MIN);
});

test('Retry-After accepts seconds and HTTP dates', () => {
  const now = Date.parse('2026-09-23T06:00:00Z');
  assert.equal(lib.parseRetryAfter('120', now), now + 120 * 1000);
  assert.equal(lib.parseRetryAfter('Wed, 23 Sep 2026 07:00:00 GMT', now), Date.parse('2026-09-23T07:00:00Z'));
  assert.equal(lib.parseRetryAfter(null, now), null);
  assert.equal(lib.parseRetryAfter('soon', now), null);
  assert.equal(lib.classifyServerError({ httpStatus: 429 }), 'rate_limited');
  assert.equal(lib.classifyServerError(new Error('unexpected status 429 Too Many Requests')), 'rate_limited');
  assert.equal(lib.classifyServerError({ httpStatus: 401 }), 'auth_required');
});

test('only usage endpoints are allowed: no thread, turn, or model requests', () => {
  assert.doesNotThrow(() => lib.assertAllowedAppServerRequest('initialize'));
  assert.doesNotThrow(() => lib.assertAllowedAppServerRequest('account/rateLimits/read'));
  for (const method of ['thread/start', 'turn/start', 'thread/resume', 'model/list', 'review/start', 'command/exec']) {
    assert.throws(() => lib.assertAllowedAppServerRequest(method), /Blocked/);
  }
  assert.throws(() => lib.assertAllowedAppServerNotification('turn/interrupt'), /Blocked/);
  assert.doesNotThrow(() => lib.assertAllowedHttpUrl('https://chatgpt.com/backend-api/wham/usage'));
  for (const url of ['https://api.openai.com/v1/responses', 'https://chatgpt.com/backend-api/codex/responses', 'https://api.anthropic.com/v1/messages']) {
    assert.throws(() => lib.assertAllowedHttpUrl(url), /Blocked/);
  }
});

test('codex executable: config, then codex.exe on PATH, then newest Codex app hash folder with codex.exe', () => {
  const files = new Set([
    'C:\\custom\\codex.exe',
    'C:\\Users\\u\\AppData\\Local\\OpenAI\\Codex\\bin\\newhash\\codex.exe',
    'C:\\Users\\u\\AppData\\Local\\OpenAI\\Codex\\bin\\oldhash\\codex.exe',
    'C:\\Users\\u\\AppData\\Local\\hermes\\node\\codex.cmd',
  ]);
  const exists = (p) => files.has(p);
  const appBinDirs = [
    { dir: 'C:\\Users\\u\\AppData\\Local\\OpenAI\\Codex\\bin\\oldhash', exeMtimeMs: 1 },
    { dir: 'C:\\Users\\u\\AppData\\Local\\OpenAI\\Codex\\bin\\emptyhash', exeMtimeMs: 3 },
    { dir: 'C:\\Users\\u\\AppData\\Local\\OpenAI\\Codex\\bin\\newhash', exeMtimeMs: 2 },
  ];
  assert.deepEqual(lib.resolveCodexExecutable({ configuredPath: 'C:\\custom\\codex.exe', exists }), { path: 'C:\\custom\\codex.exe', origin: 'config' });
  assert.equal(lib.resolveCodexExecutable({ configuredPath: 'C:\\missing\\codex.exe', exists }), null);
  assert.deepEqual(
    lib.resolveCodexExecutable({ pathDirs: ['C:\\Users\\u\\AppData\\Local\\hermes\\node', 'C:\\custom\\'], appBinDirs, exists }),
    { path: 'C:\\custom\\codex.exe', origin: 'PATH' },
    'npm shims such as codex.cmd are ignored',
  );
  assert.deepEqual(
    lib.resolveCodexExecutable({ pathDirs: ['C:\\Users\\u\\AppData\\Local\\hermes\\node'], appBinDirs, exists }),
    { path: 'C:\\Users\\u\\AppData\\Local\\OpenAI\\Codex\\bin\\newhash\\codex.exe', origin: 'Codex app' },
  );
});

test('snapshot records source, data time, plan and limit state', () => {
  const dataAtMs = Date.parse('2026-09-23T05:10:00Z');
  const snapshot = lib.buildSnapshot(
    { five_hour: null, seven_day: { used_percent: 100, window_minutes: 10080, resets_at: 1790567129 }, plan_type: null, rate_limit_reached_type: null },
    { source: 'jsonl', method: 'session-event', dataAtMs, fetchedAtMs: dataAtMs + 1000, previous: { plan_type: 'pro' } },
  );
  assert.equal(snapshot.limit_reached, true);
  assert.equal(snapshot.plan_type, 'pro');
  assert.equal(snapshot.data_at_unix, Math.floor(dataAtMs / 1000));
  assert.equal(lib.isNewerThanSnapshot(dataAtMs, snapshot), false);
  assert.equal(lib.isNewerThanSnapshot(dataAtMs + 1, snapshot), true);
});
