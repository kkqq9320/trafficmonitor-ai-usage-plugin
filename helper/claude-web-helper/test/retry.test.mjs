import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// Point the helper at an empty temporary base dir before importing it.
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tm-claude-helper-test-'));
process.env.LOCALAPPDATA = root;
delete process.env.CLAUDE_WEB_HELPER_REFRESH_MS;
const helper = await import('../index.mjs');
test.after(() => fs.rmSync(root, { recursive: true, force: true }));

const MIN = 60 * 1000;

test('importing the helper does not run a refresh', () => {
  assert.equal(fs.existsSync(path.join(root, 'trafficmonitor-claude-usage-plugin', 'claude-web-helper-status.json')), false);
});

test('Retry-After is honored for 429 responses', () => {
  const now = Date.parse('2026-09-23T06:00:00Z');
  assert.equal(helper.parseRetryAfter('3600', now), now + 60 * MIN);
  assert.equal(helper.parseRetryAfter('Wed, 23 Sep 2026 07:00:00 GMT', now), now + 60 * MIN);
  assert.equal(helper.parseRetryAfter(null, now), null);

  const refreshMs = 5 * MIN;
  assert.equal(helper.computeNextDelayMs({ state: 'ok', refreshMs, consecutiveRateLimits: 0, retryAfterAtMs: null, nowMs: now }), refreshMs);
  assert.equal(
    helper.computeNextDelayMs({ state: 'rate_limited', refreshMs, consecutiveRateLimits: 0, retryAfterAtMs: now + 60 * MIN, nowMs: now }),
    60 * MIN,
    'waits for Retry-After',
  );
  assert.equal(
    helper.computeNextDelayMs({ state: 'rate_limited', refreshMs, consecutiveRateLimits: 0, retryAfterAtMs: now + 10 * 1000, nowMs: now }),
    refreshMs,
    'never faster than the refresh interval',
  );
  assert.equal(helper.computeNextDelayMs({ state: 'rate_limited', refreshMs, consecutiveRateLimits: 0, retryAfterAtMs: null, nowMs: now }), 5 * MIN);
  assert.equal(helper.computeNextDelayMs({ state: 'rate_limited', refreshMs, consecutiveRateLimits: 2, retryAfterAtMs: null, nowMs: now }), 20 * MIN);
  assert.equal(helper.computeNextDelayMs({ state: 'rate_limited', refreshMs, consecutiveRateLimits: 8, retryAfterAtMs: null, nowMs: now }), 60 * MIN);
  assert.equal(helper.computeNextDelayMs({ state: 'request_failed', refreshMs, consecutiveRateLimits: 0, retryAfterAtMs: null, nowMs: now }), refreshMs);
});

test('Claude Code activity pulls the next fetch forward, at most once a minute', () => {
  const fetchedAt = Date.parse('2026-09-24T00:00:00Z');
  const next = (overrides) =>
    helper.computeNextFetchAtMs({
      lastFetchStartedAtMs: fetchedAt,
      pendingActivitySinceMs: null,
      state: 'ok',
      refreshMs: 5 * MIN,
      consecutiveRateLimits: 0,
      retryAfterAtMs: null,
      ...overrides,
    });

  assert.equal(next({}), Date.parse('2026-09-24T00:05:00Z'), 'idle: the regular refresh interval');
  assert.equal(
    next({ pendingActivitySinceMs: Date.parse('2026-09-24T00:00:10Z') }),
    Date.parse('2026-09-24T00:01:00Z'),
    'activity right after a fetch waits for the one-minute spacing',
  );
  assert.equal(
    next({ pendingActivitySinceMs: Date.parse('2026-09-24T00:03:00Z') }),
    Date.parse('2026-09-24T00:03:05Z'),
    'activity after a quiet minute is fetched 5 seconds later, once usage has been counted',
  );
  assert.equal(
    next({ pendingActivitySinceMs: Date.parse('2026-09-24T00:04:58Z') }),
    Date.parse('2026-09-24T00:05:00Z'),
    'never later than the regular refresh',
  );
  assert.equal(
    next({ pendingActivitySinceMs: Date.parse('2026-09-24T00:00:10Z'), state: 'request_failed' }),
    Date.parse('2026-09-24T00:01:00Z'),
    'a transient failure is retried on activity',
  );
});

test('Claude Code activity never overrides a rate limit or a sign-in problem', () => {
  const fetchedAt = Date.parse('2026-09-24T00:00:00Z');
  const activity = Date.parse('2026-09-24T00:00:10Z');
  const base = {
    lastFetchStartedAtMs: fetchedAt,
    pendingActivitySinceMs: activity,
    refreshMs: 5 * MIN,
    consecutiveRateLimits: 1,
    retryAfterAtMs: null,
  };

  assert.equal(
    helper.computeNextFetchAtMs({ ...base, state: 'rate_limited', retryAfterAtMs: Date.parse('2026-09-24T00:30:00Z') }),
    Date.parse('2026-09-24T00:30:00Z'),
    'waits for Retry-After',
  );
  assert.equal(
    helper.computeNextFetchAtMs({ ...base, state: 'rate_limited', consecutiveRateLimits: 2 }),
    Date.parse('2026-09-24T00:10:00Z'),
    'keeps the rate-limit backoff (second 429 in a row doubles the wait)',
  );
  assert.equal(
    helper.computeNextFetchAtMs({ ...base, state: 'login_required' }),
    Date.parse('2026-09-24T00:05:00Z'),
    'sign-in problems are not retried faster',
  );
});
