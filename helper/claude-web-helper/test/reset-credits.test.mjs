import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// Point the helper at an empty temporary base dir before importing it.
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tm-claude-reset-test-'));
process.env.LOCALAPPDATA = root;
const helper = await import('../index.mjs');
test.after(() => fs.rmSync(root, { recursive: true, force: true }));

// Grant shape observed 2026-09-27 from GET .../usage?cedar_ember=1.
const launchGrant = {
  id: 'opus55-launch-promax-20260921',
  label: 'Claude Opus 5.5 launch: one usage-limit reset for Pro and Max',
  resets_total: 1,
  resets_left: 1,
  starts_at: '2026-09-22T16:00:00+00:00',
  ends_at: '2026-10-22T16:00:00+00:00',
  clears: ['five_hour', 'seven_day', 'seven_day_overage_included'],
  paused: false,
  usable_now: true,
  use_requires_limit: false,
  blocking: [],
};

test('the usage request asks for reset grants without dropping other fields', () => {
  assert.equal(helper.buildUsageUrl('org-1'), 'https://claude.ai/api/organizations/org-1/usage?cedar_ember=1');
});

test('reset grants are counted the way the claude.ai settings page counts them', () => {
  assert.deepEqual(helper.summarizeResetGrants({ eligible: true, grants: [launchGrant] }), {
    available_count: 1,
    earliest_expires_at: 1792684800,
  });

  const summary = helper.summarizeResetGrants({
    eligible: true,
    grants: [
      { ...launchGrant, id: 'a', resets_left: 2, ends_at: '2026-11-01T00:00:00+00:00' },
      { ...launchGrant, id: 'paused', paused: true, resets_left: 5, ends_at: '2026-10-01T00:00:00+00:00' },
      { ...launchGrant, id: 'used-up', resets_left: 0, usable_now: false, ends_at: '2026-10-02T00:00:00+00:00' },
      { ...launchGrant, id: 'usable', resets_left: 0, usable_now: true, ends_at: '2026-10-22T16:00:00+00:00' },
    ],
  });
  assert.deepEqual(summary, { available_count: 3, earliest_expires_at: 1792684800 }, 'paused and used-up grants neither count nor set the expiry');
});

test('no reset grant information means nothing to show', () => {
  assert.equal(helper.summarizeResetGrants(null), null, 'field missing (query not honored)');
  assert.equal(helper.summarizeResetGrants({ eligible: false, ineligible_reason: 'plan', grants: [] }), null);
  assert.deepEqual(helper.summarizeResetGrants({ eligible: true, grants: [] }), { available_count: 0, earliest_expires_at: null });
});
