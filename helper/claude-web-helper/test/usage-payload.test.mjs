import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// Point the helper at an empty temporary base dir before importing it.
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tm-claude-usage-test-'));
process.env.LOCALAPPDATA = root;
const helper = await import('../index.mjs');
test.after(() => fs.rmSync(root, { recursive: true, force: true }));

// Parts of GET .../usage?cedar_ember=1 observed 2026-10-01 on a Max (5x) account.
const window = (utilization, resetsAt) => ({
  utilization, resets_at: resetsAt, limit_dollars: null, used_dollars: null, remaining_dollars: null, locked_reason: null,
});
const observed = {
  five_hour: window(11, '2026-09-30T19:20:00.141137+00:00'),
  seven_day: window(82, '2026-10-03T14:00:00.141156+00:00'),
  seven_day_opus: null,
  seven_day_sonnet: null,
  cedar_ember: {
    eligible: true,
    grants: [],
    event_props: { surface: 'claude_ai', tier: 'claude_max_5x', billing_path: 'stripe' },
  },
  extra_usage: { is_enabled: false },
  limits: [
    { kind: 'session', group: 'session', percent: 11, severity: 'normal', resets_at: '2026-09-30T19:20:00.141137+00:00', scope: null, is_active: false },
    { kind: 'weekly_all', group: 'weekly', percent: 82, severity: 'warning', resets_at: '2026-10-03T14:00:00.141156+00:00', scope: null, is_active: true },
    {
      kind: 'weekly_scoped', group: 'weekly', percent: 9, severity: 'normal', resets_at: '2026-10-03T14:00:00.141316+00:00',
      scope: { model: { id: null, display_name: 'Fable' }, surface: null }, is_active: false,
    },
  ],
};

test('the plan comes from the tier the usage response already carries', () => {
  assert.equal(helper.summarizePlan(observed.cedar_ember), 'Max (5x)');
  assert.equal(helper.summarizePlan({ event_props: { tier: 'claude_max_20x' } }), 'Max (20x)');
  assert.equal(helper.summarizePlan({ event_props: { tier: 'claude_pro' } }), 'Pro');
  assert.equal(helper.summarizePlan({ event_props: { tier: 'mystery tier' } }), null, 'an unknown shape is not guessed');
  assert.equal(helper.summarizePlan({ eligible: false }), null);
  assert.equal(helper.summarizePlan(null), null, 'cedar_ember missing');
});

test('model-scoped weekly limits are kept with their name, percentage and reset time', () => {
  assert.deepEqual(helper.summarizeModelLimits(observed.limits), [
    { name: 'Fable', utilization: 9, resets_at: '2026-10-03T14:00:00.141316+00:00' },
  ]);
  assert.deepEqual(helper.summarizeModelLimits([
    { kind: 'weekly_scoped', percent: 5, scope: { model: { display_name: '' } } },
    { kind: 'weekly_scoped', percent: 'x', scope: { model: { display_name: 'Broken' } } },
    { kind: 'weekly_scoped', percent: 30, resets_at: null, scope: { model: { display_name: 'Opus' } } },
  ]), [{ name: 'Opus', utilization: 30, resets_at: null }], 'entries without a name or a number are skipped');
  assert.deepEqual(helper.summarizeModelLimits(undefined), []);
});

test('the usage snapshot stores the plan and the model limits next to the windows', () => {
  const payload = helper.normalizeUsagePayload(observed);
  assert.equal(payload.plan, 'Max (5x)');
  assert.deepEqual(payload.seven_day_models, [{ name: 'Fable', utilization: 9, resets_at: '2026-10-03T14:00:00.141316+00:00' }]);
  assert.equal(payload.five_hour.utilization, 11);
  assert.equal(payload.seven_day.utilization, 82);
});
