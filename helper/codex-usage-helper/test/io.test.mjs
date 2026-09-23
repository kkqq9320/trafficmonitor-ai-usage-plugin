// File-system level tests for the Codex usage helper. Everything runs inside a temporary
// LOCALAPPDATA / CODEX_HOME; no Codex process is started and no real network request is made.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tm-codex-helper-test-'));
process.env.LOCALAPPDATA = path.join(root, 'local');
process.env.CODEX_HOME = path.join(root, 'codex');
const sessionsDay = path.join(process.env.CODEX_HOME, 'sessions', '2026', '09', '23');
const baseDir = path.join(process.env.LOCALAPPDATA, 'trafficmonitor-claude-usage-plugin');
const snapshotPath = path.join(baseDir, 'codex-usage.json');
const statusPath = path.join(baseDir, 'codex-usage-helper-status.json');
fs.mkdirSync(sessionsDay, { recursive: true });
fs.writeFileSync(path.join(process.env.CODEX_HOME, 'auth.json'), JSON.stringify({ auth_mode: 'chatgpt', tokens: { access_token: 'test-access-token', account_id: 'test-account' } }));

const helperModule = await import('../index.mjs');
const realFetch = globalThis.fetch;
test.after(() => {
  globalThis.fetch = realFetch;
  fs.rmSync(root, { recursive: true, force: true });
});

const MIN = 60 * 1000;
const iso = (ms) => new Date(ms).toISOString();
const tokenCount = (ms, rateLimits) => JSON.stringify({ timestamp: iso(ms), type: 'event_msg', payload: { type: 'token_count', info: null, rate_limits: rateLimits } }) + '\n';
const weekly = (used, limitId = 'codex') => ({ limit_id: limitId, primary: { used_percent: used, window_minutes: 10080, resets_at: 1790567129 }, secondary: null });
const setMtime = (file, ms) => fs.utimesSync(file, new Date(ms), new Date(ms));
const readJson = (file) => JSON.parse(fs.readFileSync(file, 'utf8'));
async function waitFor(predicate, timeoutMs = 5000) {
  const until = Date.now() + timeoutMs;
  while (Date.now() < until) {
    if (predicate()) return true;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  return predicate();
}

test('JSONL scan picks the newest event by timestamp, not by file mtime', () => {
  const now = Date.now();
  // An old session whose mtime was refreshed (reopened today) but whose last codex event is old.
  const touched = path.join(sessionsDay, 'rollout-touched.jsonl');
  fs.writeFileSync(touched, tokenCount(now - 180 * MIN, weekly(89)));
  setMtime(touched, now);
  // A session that is still open: Windows kept its mtime at open time although newer events exist.
  const open = path.join(sessionsDay, 'rollout-open.jsonl');
  fs.writeFileSync(open, tokenCount(now - 40 * MIN, weekly(90)) + tokenCount(now - 31 * MIN, weekly(94)));
  setMtime(open, now - 120 * MIN);
  // A newer Anthropic-routed session: codex bucket with null windows plus other buckets.
  const routed = path.join(sessionsDay, 'rollout-routed.jsonl');
  fs.writeFileSync(
    routed,
    tokenCount(now - 5 * MIN, { limit_id: 'codex', primary: null, secondary: null }) +
      tokenCount(now - 4 * MIN, { limit_id: 'codex_bengalfox', primary: { used_percent: 12, window_minutes: 300, resets_at: 1 }, secondary: { used_percent: 70, window_minutes: 10080, resets_at: 2 } }) +
      tokenCount(now - 3 * MIN, { limit_id: 'premium', primary: null, secondary: null }),
  );
  setMtime(routed, now - 1 * MIN);

  const event = helperModule.findLatestJsonlEvent();
  assert.equal(event.seven_day.used_percent, 94);
  assert.equal(event.five_hour, null);
  assert.equal(event.timestampMs, now - 31 * MIN);
});

test('JSONL scan reads only the tail of files larger than 32 MB', () => {
  const now = Date.now();
  const big = path.join(sessionsDay, 'rollout-big.jsonl');
  const filler = JSON.stringify({ timestamp: iso(now - 60 * MIN), type: 'response_item', payload: { type: 'message', text: 'x'.repeat(1024 * 1024) } }) + '\n';
  const fd = fs.openSync(big, 'w');
  for (let index = 0; index < 33; index += 1) fs.writeSync(fd, filler);
  fs.writeSync(fd, tokenCount(now - 10 * MIN, weekly(55)));
  fs.closeSync(fd);
  assert.ok(fs.statSync(big).size > 32 * 1024 * 1024);
  const started = Date.now();
  const event = helperModule.findLatestJsonlEvent();
  assert.equal(event.seven_day.used_percent, 55);
  assert.ok(Date.now() - started < 5000);
  fs.rmSync(big);
});

test('wham/usage: sends the ChatGPT token only to the usage URL and honors 429 Retry-After', async () => {
  const calls = [];
  const fakeFetch = async (url, init) => {
    calls.push({ url, headers: init.headers });
    return new Response('{"detail":"slow down"}', { status: 429, headers: { 'retry-after': '120' } });
  };
  const before = Date.now();
  await assert.rejects(helperModule.readRateLimitsViaWham(fakeFetch), (error) => {
    assert.equal(error.httpStatus, 429);
    assert.ok(error.retryAfterMs >= before + 119 * 1000 && error.retryAfterMs <= Date.now() + 121 * 1000);
    return true;
  });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, 'https://chatgpt.com/backend-api/wham/usage');
  assert.equal(calls[0].headers.authorization, 'Bearer test-access-token');
  assert.equal(calls[0].headers['chatgpt-account-id'], 'test-account');
});

test('server path: rate limit falls back to JSONL, keeps Retry-After; success writes a server snapshot', async () => {
  const config = { codexPath: path.join(root, 'missing', 'codex.exe'), serverRefreshMs: 15 * MIN };
  let requests = 0;
  globalThis.fetch = async () => {
    requests += 1;
    return new Response('{}', { status: 429, headers: { 'retry-after': '600' } });
  };
  const limited = new helperModule.CodexUsageHelper(config);
  assert.equal(await limited.fetchFromServer(), false);
  assert.equal(requests, 1);
  const status = readJson(statusPath);
  assert.equal(status.state, 'rate_limited');
  assert.ok(Date.parse(status.server.retry_after_until) >= Date.now() + 590 * 1000);
  assert.ok(limited.nextServerFetchMs() >= Date.parse(status.server.retry_after_until));
  let snapshot = readJson(snapshotPath);
  assert.equal(snapshot.source, 'jsonl');
  assert.equal(snapshot.seven_day.used_percent, 94);

  globalThis.fetch = async () =>
    new Response(JSON.stringify({
      plan_type: 'pro',
      rate_limit: { limit_reached: true, primary_window: { used_percent: 100, limit_window_seconds: 604800, reset_at: 1790567129 }, secondary_window: null },
      rate_limit_reached_type: { type: 'rate_limit_reached', details: 'default' },
    }), { status: 200, headers: { 'content-type': 'application/json' } });
  const ok = new helperModule.CodexUsageHelper(config);
  ok.server.retryAfterUntilMs = null;
  assert.equal(await ok.fetchFromServer(), true);
  snapshot = readJson(snapshotPath);
  assert.equal(snapshot.source, 'server');
  assert.equal(snapshot.method, 'wham');
  assert.equal(snapshot.seven_day.used_percent, 100);
  assert.equal(snapshot.limit_reached, true);
  assert.equal(snapshot.rate_limit_reached_type, 'rate_limit_reached');
  assert.equal(snapshot.plan_type, 'pro');
  assert.equal(readJson(statusPath).state, 'ok');
  globalThis.fetch = realFetch;
});

test('push path: a new codex event in a session file updates the snapshot within seconds', async () => {
  const helper = new helperModule.CodexUsageHelper({ codexPath: null, serverRefreshMs: 15 * MIN });
  helper.startSessionWatcher();
  try {
    const live = path.join(sessionsDay, 'rollout-live.jsonl');
    fs.writeFileSync(live, '');
    await new Promise((resolve) => setTimeout(resolve, 300));
    const eventMs = Date.now();
    fs.appendFileSync(live, tokenCount(eventMs - 1000, { limit_id: 'codex', primary: null, secondary: null }));
    fs.appendFileSync(live, tokenCount(eventMs, { limit_id: 'codex', primary: { used_percent: 3, window_minutes: 300, resets_at: 1790000000 }, secondary: { used_percent: 1, window_minutes: 10080, resets_at: 1790500000 } }));
    const updated = await waitFor(() => {
      try {
        const snapshot = readJson(snapshotPath);
        return snapshot.method === 'session-event' && snapshot.five_hour && snapshot.five_hour.used_percent === 3;
      } catch {
        return false;
      }
    });
    assert.ok(updated, 'snapshot was not updated from the session event');
    const snapshot = readJson(snapshotPath);
    assert.equal(snapshot.seven_day.used_percent, 1);
    assert.equal(snapshot.limit_reached, false);
    assert.equal(snapshot.plan_type, 'pro', 'plan type is kept from the previous server snapshot');

    assert.equal(helper.server.requestedAtMs, null);
    fs.appendFileSync(live, JSON.stringify({ timestamp: iso(Date.now()), type: 'event_msg', payload: { type: 'task_complete', error: { message: 'limit', codex_error_info: 'usage_limit_exceeded' } } }) + '\n');
    assert.ok(await waitFor(() => helper.server.requestedAtMs !== null), 'usage limit event did not request a server refresh');
  } finally {
    helper.stopSessionWatcher();
  }
});
