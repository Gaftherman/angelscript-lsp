import path from 'node:path';
import { DEBUGGER_SCENARIO, holdDebuggerFrame } from './record-debugger.mjs';

export const DEBUGGER_ENGINE_SCENARIO = {
  ...DEBUGGER_SCENARIO,
  file: 'scripts/engine.as',
  sidebarWidth: 480,
  setup: { tab: 'scripts/host.as.predefined' },
  editorSettings: {
    ...DEBUGGER_SCENARIO.editorSettings,
    'debug.allowBreakpointsEverywhere': true,
  },
};

const waitVisible = locator => locator.waitFor({ timeout: 5000 });
const editor = win => win.locator('.part.editor .monaco-editor:visible').first();
const scriptLine = (win, text) => editor(win).locator('.view-line').filter({ hasText: text });

export async function prepareDebuggerEngine(win) {
  await win.addStyleTag({ content: `
    .part.sidebar .debug-call-stack .stack-frame > .file:not(:first-child) {
      margin-inline-start: 4px;
    }
    .part.sidebar .debug-call-stack .stack-frame > .file > .file-name {
      margin-inline-end: 4px;
    }
  ` });
  const callback = await scriptLine(win, /lastHealth\s*=\s*player\.health;.*\/\/\s*BP/).boundingBox();
  const gutter = await editor(win).locator('.glyph-margin').boundingBox();
  if (!callback || !gutter) throw new Error('Could not find the callback breakpoint gutter.');
  await win.mouse.click(gutter.x + gutter.width / 2, callback.y + callback.height / 2);
  await scriptLine(win, /print\(message\).*\/\/\s*BP/).click({ position: { x: 6, y: 12 } });
}

export async function runDebuggerEngineSteps({ win, animClick, press, firstActionTime }) {
  const sidebar = win.locator('.part.sidebar');
  const frames = sidebar.locator('.debug-call-stack .stack-frame');
  const rows = sidebar.locator('.debug-variables .monaco-list-row');
  const highlight = editor(win).locator('.debug-top-stack-frame-line').first();
  const player = rows.filter({ hasText: /\bplayer\b/ });
  const samples = [];

  const hold = async (name, ms) => {
    const time = (Date.now() - firstActionTime + ms / 2) / 1000;
    samples.push({ name, time });
    console.log(`Review ${name} at ${time.toFixed(2)} s`);
    await win.waitForTimeout(ms);
  };
  const waitForFrame = async (index, name, line) => {
    const frame = frames.nth(index);
    await waitVisible(frame.getByText(name));
    const filename = frame.locator('.file-name').filter({ hasText: /^engine\.as$/ });
    await waitVisible(frame.locator('.line-number').filter({ hasText: new RegExp(`^${line}:[0-9]+$`) }));
    await waitVisible(filename);
    if (await filename.evaluate(label => label.scrollWidth > label.clientWidth)) {
      throw new Error(`The Call Stack clips engine.as at line ${line}.`);
    }
  };
  const reveal = async (key, waitForResult) => {
    await holdDebuggerFrame(win);
    await press(key, async () => {
      await waitForResult();
      await win.locator('#demo-debugger-frame').evaluate(frame => frame.remove());
    });
  };
  const click = async locator => {
    await waitVisible(locator);
    const box = await locator.boundingBox();
    if (!box) throw new Error('The debugger control is outside the window.');
    await animClick({ x: box.x + box.width / 2, y: box.y + box.height / 2 });
  };

  await win.waitForTimeout(1500);
  await press('F9', () => editor(win).locator('.glyph-margin-widgets .codicon-debug-breakpoint').nth(1)
    .waitFor({ timeout: 5000 }));
  await hold('breakpoint', 1650);

  await reveal('F5', async () => {
    await waitVisible(highlight);
    await waitForFrame(0, /healPlayer\(/, 6);
    await waitForFrame(1, /OnPlayerSpawn\(/, 14);
    await waitVisible(rows.filter({ hasText: /Healing Mira/ }));
    if (!(await player.isVisible())) await sidebar.getByText('Parameters', { exact: true }).click();
    await waitVisible(rows.filter({ hasText: /\bamount\b/ }));
    await waitVisible(player);
  });
  await win.waitForTimeout(1250);
  await click(player.locator('.monaco-tl-twistie'));
  const health = rows.filter({ hasText: /\bhealth\b/ }).locator('.value');
  await waitVisible(health.filter({ hasText: /^\s*65\s*$/ }));
  await hold('native-call', 2000);

  await reveal('F11', async () => {
    await waitVisible(highlight);
    await waitForFrame(0, /healPlayer\(/, 7);
    await waitForFrame(1, /OnPlayerSpawn\(/, 14);
  });
  await hold('step-in', 1900);

  await reveal('F5', async () => {
    await waitVisible(highlight);
    await waitForFrame(0, /OnPlayerSpawn\(/, 15);
    await frames.nth(1).waitFor({ state: 'hidden', timeout: 5000 });
    await waitVisible(player);
  });
  await hold('callback', 2200);

  await click(win.locator('.part.editor .tab').filter({ hasText: 'host.as.predefined' }));
  await scriptLine(win, /void\s+print\(/).click({ position: { x: 6, y: 12 } });
  await win.waitForTimeout(650);
  await press('F9', () => waitVisible(editor(win).locator('.glyph-margin-widgets .codicon-debug-breakpoint-unverified').first()));
  await hold('unverified', 2200);
  return samples;
}

export function writeDebuggerEngineReview(ffmpeg, gif, { duration, reviewDir, samples }) {
  ffmpeg(['-i', gif, '-vf', `fps=12/${duration}:round=up,scale=480:-1,tile=3x4`,
    '-frames:v', '1', '-update', '1', path.join(reviewDir, 'debugger-engine.png')]);
  for (const { name, time } of samples) {
    ffmpeg(['-ss', time.toFixed(3), '-i', gif, '-frames:v', '1', '-update', '1',
      path.join(reviewDir, `debugger-engine-${name}.png`)]);
  }
}
