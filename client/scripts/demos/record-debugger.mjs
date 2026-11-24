import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { createInterface } from 'node:readline';

export const DEBUGGER_WORKSPACE_DIR = 'C:\\demo\\debug-example';

export const DEBUGGER_SCENARIO = {
  file: 'scripts/basic.as',
  wholeLineMargin: 1,
  keycapHoldMs: 1200,
  editorSettings: {
    'debug.internalConsoleOptions': 'neverOpen',
    'debug.toolBarLocation': 'docked',
    'editor.codeLens': false,
    'editor.fontSize': 22,
    'editor.lineHeight': 26,
    'editor.padding.top': 14,
    'editor.padding.bottom': 14,
    'editor.glyphMargin': true,
    'editor.hover.enabled': false,
    'editor.inlayHints.enabled': 'off',
    'editor.occurrencesHighlight': 'off',
    'editor.renderValidationDecorations': 'off',
    'editor.scrollBeyondLastLine': false,
    'workbench.editor.decorations.badges': false,
    'workbench.editor.decorations.colors': false,
    'workbench.activityBar.location': 'default',
  },
  steps: [],
};

export function copyDebuggerWorkspace(exampleDir) {
  const parent = path.dirname(DEBUGGER_WORKSPACE_DIR);
  fs.mkdirSync(parent, { recursive: true });
  if (path.relative(parent, fs.realpathSync(parent))) {
    throw new Error('The neutral demo directory resolves outside its expected location.');
  }
  fs.mkdirSync(DEBUGGER_WORKSPACE_DIR);
  const clean = () => {
    if (path.relative(DEBUGGER_WORKSPACE_DIR, fs.realpathSync(DEBUGGER_WORKSPACE_DIR))) {
      throw new Error('The debugger workspace resolves outside its reserved location.');
    }
    fs.rmSync(DEBUGGER_WORKSPACE_DIR, { recursive: true, maxRetries: 3, retryDelay: 150 });
  };
  try {
    for (const directory of ['.vscode', 'scripts']) {
      fs.cpSync(path.join(exampleDir, directory), path.join(DEBUGGER_WORKSPACE_DIR, directory), { recursive: true });
    }
  } catch (error) {
    clean();
    throw error;
  }
  return clean;
}

export function launchDebuggerHost(exampleDir) {
  const executable = path.join(exampleDir, 'build/bin/Release/angelscript-demo.exe');
  if (!fs.existsSync(executable)) {
    throw new Error('Build the debugger example with run-demo.cmd --build-only before recording.');
  }

  const child = spawn(executable, ['--wait'], {
    cwd: DEBUGGER_WORKSPACE_DIR,
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const closed = new Promise(resolve => child.once('close', resolve));
  let readyTimeout;
  const ready = new Promise((resolve, reject) => {
    readyTimeout = setTimeout(() => reject(new Error('The debugger host did not start listening.')), 5000);
    createInterface({ input: child.stdout }).on('line', line => {
      if (line === 'Waiting for debugger configuration...') resolve();
    });
    child.stderr.resume();
    child.once('error', reject);
    closed.then(code => reject(new Error(`The debugger host exited before attaching (code ${code}).`)));
  }).finally(() => clearTimeout(readyTimeout));

  return {
    ready,
    async stop() {
      if (child.exitCode === null && child.signalCode === null) child.kill();
      await closed;
    },
  };
}

export function writeDebuggerCompatibilityExtension(userDataDir) {
  const extensionDir = path.join(userDataDir, 'debugger-demo-compatibility');
  fs.mkdirSync(extensionDir);
  fs.writeFileSync(path.join(extensionDir, 'package.json'), JSON.stringify({
    name: 'debugger-demo-compatibility',
    publisher: 'angelscript-demo',
    version: '0.0.1',
    engines: { vscode: '^1.146.0' },
    activationEvents: ['onDebugAdapterProtocolTracker:angelscript'],
    main: './debugger-compat.cjs',
  }));
  fs.copyFileSync(new URL('./debugger-compat.cjs', import.meta.url), path.join(extensionDir, 'debugger-compat.cjs'));
  return extensionDir;
}

export async function prepareDebuggerWindow(win, sidebarWidth = 408) {
  await win.addStyleTag({ content: `
    #demo-debugger-frame {
      position: fixed;
      inset: 0;
      width: 100%;
      height: 100%;
      z-index: 2147483645;
      pointer-events: none;
    }
    #demo-debugger-frame ~ #demo-cursor {
      visibility: hidden;
    }
    .part.sidebar .debug-pane .monaco-list-row,
    .part.sidebar .debug-pane .debug-variables .expression,
    .part.sidebar .debug-pane .debug-call-stack .stack-frame {
      font-size: 17px;
    }
    .part.sidebar .debug-pane .scope {
      font-size: 15px;
    }
    .part.sidebar .debug-pane .call-stack-state-message > .label {
      font-size: 12px;
    }
    .part.sidebar > .title.has-start-debug-action-item > .title-label {
      flex: 0 0 auto;
    }
    .part.sidebar .start-debug-action-item .configuration .monaco-dropdown > .dropdown-label .start-debug-action-item-label {
      font-size: 15px;
    }
    .part.sidebar .debug-pane .pane-header .title {
      font-size: 15px;
    }
    .part.sidebar .debug-pane .debug-call-stack .line-number {
      font-size: 14px;
    }
    .notifications-toasts, .part.editor .decorationsOverviewRuler {
      display: none !important;
    }
    .part.editor .monaco-editor .squiggly-error {
      background: none;
      border: none;
    }
  ` });
  await win.keyboard.press('Control+Shift+d');
  const sidebar = win.locator('.part.sidebar');
  await sidebar.getByText('Variables', { exact: true }).waitFor({ timeout: 5000 });
  const panel = await sidebar.boundingBox();
  const edge = panel.x + panel.width;
  const sash = await win.locator('.monaco-sash.vertical:visible').evaluateAll((sashes, edge) =>
    sashes.map(element => element.getBoundingClientRect())
      .find(rect => Math.abs(rect.left + rect.width / 2 - edge) < 4)?.toJSON(), edge);
  if (!sash) throw new Error('Could not find the Run and Debug sidebar divider.');
  const y = sash.y + sash.height / 2;
  await win.mouse.move(edge, y);
  await win.mouse.down();
  await win.mouse.move(panel.x + sidebarWidth, y, { steps: 8 });
  await win.mouse.up();
  await sidebar.getByText('Attach to AngelScript Demo', { exact: true }).first().waitFor({ timeout: 5000 });
  await sidebar.getByText('Watch', { exact: true }).click();
  await win.locator('.part.editor .view-line:visible').first().click({ position: { x: 4, y: 11 } });
}

export async function holdDebuggerFrame(win) {
  // Cover the workbench while VS Code refreshes the editor, Locals and Call Stack independently.
  const screenshot = await win.screenshot();
  await win.evaluate(async png => {
    const bytes = Uint8Array.from(atob(png), character => character.charCodeAt(0));
    const bitmap = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
    const frame = document.createElement('canvas');
    frame.id = 'demo-debugger-frame';
    frame.setAttribute('aria-hidden', 'true');
    frame.width = bitmap.width;
    frame.height = bitmap.height;
    frame.getContext('2d').drawImage(bitmap, 0, 0);
    bitmap.close();
    document.getElementById('demo-cursor').before(frame);
  }, screenshot.toString('base64'));
}

export async function runDebuggerSteps({ win, animClick, animMoveMouse, press, firstActionTime }) {
  const editor = win.locator('.part.editor .monaco-editor:visible').first();
  const bonusLine = editor.locator('.view-line').filter({ hasText: /total\s*=\s*addBonus.*\/\/\s*BP/ });
  const breakpoint = editor.locator('.glyph-margin-widgets .codicon-debug-breakpoint').first();
  const highlight = editor.locator('.debug-top-stack-frame-line').first();
  const stack = win.locator('.debug-call-stack');
  const frames = stack.locator('.stack-frame');
  const variables = win.locator('.debug-variables .expression');
  const total = variables.filter({ hasText: /\btotal\b/ }).locator('.value');
  const prefix = variables.filter({ hasText: /\bprefix\b/ }).locator('.value');
  const waitVisible = locator => locator.waitFor({ timeout: 5000 });
  const waitHidden = locator => locator.waitFor({ state: 'hidden', timeout: 5000 });
  const waitForCaller = async (index, line) => {
    await waitVisible(frames.nth(index).getByText(/showProgress\(/));
    await waitVisible(frames.nth(index).locator('.line-number').filter({ hasText: line }));
    await waitVisible(frames.nth(index + 1).getByText(/main\(/));
    await waitHidden(stack.getByText('Load More Stack Frames', { exact: true }));
  };
  const pressAndReveal = async (key, waitForResult) => {
    await holdDebuggerFrame(win);
    await press(key, async () => {
      await waitForResult();
      await win.locator('#demo-debugger-frame').evaluate(frame => frame.remove());
    });
  };
  const gutterPoint = async () => {
    const line = await bonusLine.boundingBox({ timeout: 5000 });
    const margin = await editor.locator('.glyph-margin').boundingBox({ timeout: 5000 });
    if (!line || !margin) throw new Error('Could not locate the addBonus breakpoint gutter.');
    return { x: margin.x + margin.width / 2, y: line.y + line.height / 2 };
  };
  const elapsed = () => (Date.now() - firstActionTime) / 1000;

  await win.waitForTimeout(1800);
  await animClick(await gutterPoint());
  await waitVisible(breakpoint);
  await animMoveMouse({ x: 1140, y: 420 });
  await win.waitForTimeout(1250);

  await pressAndReveal('F5', async () => {
    await waitVisible(highlight);
    await waitVisible(total.filter({ hasText: /^\s*7\s*$/ }));
    await waitForCaller(0, /^21:/);
  });
  const stopStart = elapsed();
  console.log('Debugger attached and paused at addBonus with total = 7.');
  await win.waitForTimeout(2700);

  await pressAndReveal('F10', async () => {
    await waitVisible(highlight);
    await waitVisible(total.filter({ hasText: /^\s*9\s*$/ }));
    await waitForCaller(0, /^22:/);
  });
  console.log('Step Over updated total to 9.');
  await win.waitForTimeout(2700);

  await pressAndReveal('F11', async () => {
    await waitVisible(highlight);
    await waitVisible(frames.first().getByText(/formatProgress\(/));
    await waitVisible(frames.first().locator('.line-number').filter({ hasText: /^8:/ }));
    await waitVisible(prefix);
    await waitForCaller(1, /^22:/);
  });
  console.log('Step Into added the formatProgress stack frame.');
  await win.waitForTimeout(1750);

  await pressAndReveal('F10', async () => {
    await waitVisible(highlight);
    await waitVisible(frames.first().getByText(/formatProgress\(/));
    await waitVisible(frames.first().locator('.line-number').filter({ hasText: /^9:/ }));
    await waitVisible(prefix.filter({ hasText: /^\s*"Total: "\s*$/ }));
    await waitForCaller(1, /^22:/);
  });
  console.log('Step Over initialized prefix to "Total: ".');
  await win.waitForTimeout(2750);
  const stopEnd = elapsed();

  // Remove the breakpoint before continuing, since Tick calls showProgress again.
  await animClick(await gutterPoint());
  await waitHidden(breakpoint);
  await animMoveMouse({ x: 1140, y: 420 });
  await win.waitForTimeout(750);
  await pressAndReveal('F5', async () => {
    await waitHidden(highlight);
    await waitHidden(frames.first());
    await waitHidden(variables.first());
    await waitVisible(win.locator('.part.sidebar').getByText('Running', { exact: true }));
  });
  await win.waitForTimeout(2000);
  return { stopStart, stopEnd };
}

export function writeDebuggerContactSheets(ffmpeg, gif, { duration, reviewDir, stopStart, stopEnd }) {
  const writeSheet = (input, filter, name) => ffmpeg([
    ...input, '-vf', filter, '-frames:v', '1', '-update', '1', path.join(reviewDir, name),
  ]);
  writeSheet(['-i', gif], `fps=12/${duration}:round=up,scale=480:-1,tile=3x4`, 'debugger.png');
  const stopDuration = (stopEnd - stopStart).toFixed(3);
  writeSheet(['-ss', stopStart.toFixed(3), '-t', stopDuration, '-i', gif],
    `fps=6/${stopDuration}:round=up,tile=2x3`, 'debugger-stop.png');
}

export function writeDebuggerReview(ffmpeg, gif, { duration, reviewDir, samples, name }) {
  ffmpeg(['-i', gif, '-vf', `fps=12/${duration}:round=up,scale=480:-1,tile=3x4`,
    '-frames:v', '1', '-update', '1', path.join(reviewDir, `${name}.png`)]);
  for (const { name: sample, time } of samples) {
    ffmpeg(['-ss', time.toFixed(3), '-i', gif, '-frames:v', '1', '-update', '1',
      path.join(reviewDir, `${name}-${sample}.png`)]);
  }
}
