import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// Point the helper at an empty temporary base dir before importing it.
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tm-claude-activity-test-'));
process.env.LOCALAPPDATA = path.join(root, 'local');
const helper = await import('../index.mjs');
test.after(() => fs.rmSync(root, { recursive: true, force: true }));

function waitForActivity(counter, timeoutMs) {
  return new Promise((resolve) => {
    const started = Date.now();
    const poll = setInterval(() => {
      if (counter.count > 0 || Date.now() - started > timeoutMs) {
        clearInterval(poll);
        resolve(counter.count);
      }
    }, 25);
  });
}

test('Claude Code transcripts live under CLAUDE_CONFIG_DIR, else %USERPROFILE%\\.claude', () => {
  assert.equal(helper.resolveClaudeProjectsDir({ USERPROFILE: 'C:\\Users\\u' }), 'C:\\Users\\u\\.claude\\projects');
  assert.equal(
    helper.resolveClaudeProjectsDir({ USERPROFILE: 'C:\\Users\\u', CLAUDE_CONFIG_DIR: 'D:\\claude-config' }),
    'D:\\claude-config\\projects',
  );
});

test('a transcript append in a nested project folder counts as activity', async () => {
  const projects = path.join(root, 'append', 'projects');
  const transcript = path.join(projects, 'C--work-repo', 'session.jsonl');
  fs.mkdirSync(path.dirname(transcript), { recursive: true });
  fs.writeFileSync(transcript, '{"type":"user"}\n');

  const counter = { count: 0 };
  const watcher = helper.watchClaudeActivity(projects, () => (counter.count += 1));
  try {
    await new Promise((resolve) => setTimeout(resolve, 200));
    fs.appendFileSync(transcript, '{"type":"assistant"}\n');
    assert.ok((await waitForActivity(counter, 3000)) > 0, 'append was not reported');
  } finally {
    watcher.close();
  }
});

test('files other than transcripts are ignored', async () => {
  const projects = path.join(root, 'other', 'projects');
  fs.mkdirSync(path.join(projects, 'C--work-repo'), { recursive: true });

  const counter = { count: 0 };
  const watcher = helper.watchClaudeActivity(projects, () => (counter.count += 1));
  try {
    await new Promise((resolve) => setTimeout(resolve, 200));
    fs.writeFileSync(path.join(projects, 'C--work-repo', 'notes.txt'), 'x');
    assert.equal(await waitForActivity(counter, 700), 0);
  } finally {
    watcher.close();
  }
});

test('a projects folder created after start is picked up on retry', async () => {
  const projects = path.join(root, 'late', 'projects');

  const counter = { count: 0 };
  const watcher = helper.watchClaudeActivity(projects, () => (counter.count += 1), { retryMs: 100 });
  try {
    fs.mkdirSync(path.join(projects, 'C--work-repo'), { recursive: true });
    await new Promise((resolve) => setTimeout(resolve, 400));
    fs.writeFileSync(path.join(projects, 'C--work-repo', 'session.jsonl'), '{"type":"user"}\n');
    assert.ok((await waitForActivity(counter, 3000)) > 0, 'folder created later was never watched');
  } finally {
    watcher.close();
  }
});
