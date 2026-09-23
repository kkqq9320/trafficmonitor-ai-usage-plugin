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
