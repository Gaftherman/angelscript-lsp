import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { DEBUGGER_SCENARIO, prepareDebuggerWindow } from './record-debugger.mjs';

const launchName = 'Launch AngelScript Demo';
const breakpoint = 'total = addBonus(total, hasBonus ? 2 : 0); // BP';
const waitVisible = (locator, timeout = 5000) => locator.waitFor({ timeout });

export const DEBUGGER_LAUNCH_SCENARIO = {
  ...DEBUGGER_SCENARIO,
  keycapGapMs: 180,
  editorSettings: {
    ...DEBUGGER_SCENARIO.editorSettings,
    'debug.console.fontSize': 20,
    'debug.console.lineHeight': 26,
  },
};

export function buildDebuggerLaunchWorkspace(exampleDir, workspaceDir) {
  const exampleBuild = path.join(exampleDir, 'build');
  if (!fs.existsSync(path.join(exampleBuild, 'bin/Release/angelscript-demo.exe'))) {
    throw new Error('Build the Release host in the example checkout with run-demo.cmd --build-only first.');
  }
  const cache = fs.readFileSync(path.join(exampleBuild, 'CMakeCache.txt'), 'utf8');
  const cachedPath = name => cache.match(new RegExp(`^${name}:PATH=(.+)$`, 'm'))?.[1].trim() || undefined;
  const sourceDebugger = cachedPath('ASDEBUGGER_DIR') ?? path.join(exampleDir, 'external/asdebugger');
  const sourceCppdap = cachedPath('FETCHCONTENT_SOURCE_DIR_CPPDAP') ?? path.join(exampleBuild, '_deps/cppdap-src');
  const copy = (source, destination) => {
    fs.cpSync(source, path.join(workspaceDir, destination), {
      recursive: true,
      filter: source => path.basename(source) !== '.git',
    });
  };
  for (const entry of ['CMakeLists.txt', 'run-demo.cmd', 'run-demo.ps1', 'host', 'external/angelscript']) {
    copy(path.join(exampleDir, entry), entry);
  }
  for (const entry of ['CMakeLists.txt', 'scripts/patch_cppdap.py', 'src/core']) {
    copy(path.join(sourceDebugger, entry), `external/asdebugger/${entry}`);
  }

  // CMake build trees contain absolute paths, so configure and warm the copy in place.
  const cppdap = 'build/_deps/cppdap-src';
  copy(sourceCppdap, cppdap);
  const cppdapPath = path.join(workspaceDir, cppdap).replaceAll('\\', '/');
  fs.writeFileSync(path.join(workspaceDir, 'build/CMakeCache.txt'), `FETCHCONTENT_SOURCE_DIR_CPPDAP:PATH=${cppdapPath}\n`);
  const options = { cwd: workspaceDir, stdio: 'inherit', windowsHide: true };
  execFileSync('git', ['init', '--quiet'], options);
  execFileSync('cmd.exe', ['/d', '/c', '.\\run-demo.cmd', '--build-only'], options);
}

async function ensurePortAvailable() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', error => {
      reject(new Error(`Port 9002 must be available before launching the demo (${error.code}).`));
    });
    server.listen({ host: '127.0.0.1', port: 9002, exclusive: true }, () => server.close(resolve));
  });
}

export async function prepareDebuggerLaunch(win, workspaceDir) {
  await prepareDebuggerWindow(win);
  await win.keyboard.press('Control+b');

  const source = fs.readFileSync(path.join(workspaceDir, DEBUGGER_LAUNCH_SCENARIO.file), 'utf8');
  const line = source.split(/\r?\n/).findIndex(text => text.trim() === breakpoint) + 1;
  if (!line) throw new Error('The launch breakpoint is missing from basic.as.');
  await win.keyboard.press('Control+g');
  await win.keyboard.type(String(line));
  await win.keyboard.press('Enter');
}

export async function runDebuggerLaunchSteps({ win, animClick, animMoveMouse, press, firstActionTime }) {
  const sidebar = win.locator('.part.sidebar');
  const editor = win.locator('.part.editor .monaco-editor:visible').first();
  const highlight = editor.locator('.debug-top-stack-frame-line').first();
  const configuration = sidebar.locator('.start-debug-action-item .configuration .dropdown-label');
  const selectedLaunch = configuration.getByText(launchName, { exact: true });
  const variables = sidebar.locator('.debug-variables .monaco-list-row');
  const consoleOutput = win.locator('.part.panel .repl .monaco-list');
  const samples = [];

  const hold = async (name, ms) => {
    const time = (Date.now() - firstActionTime + ms / 4) / 1000;
    samples.push({ name, time });
    console.log(`Review ${name} at ${time.toFixed(2)} s`);
    await win.waitForTimeout(ms);
  };
  const click = async locator => {
    await waitVisible(locator);
    const box = await locator.boundingBox();
    if (!box) throw new Error('The debugger control is outside the window.');
    await animClick({ x: box.x + box.width / 2, y: box.y + box.height / 2 });
  };

  await ensurePortAvailable();
  // Leave enough lead-in for the gray F9 keycap after the video is trimmed.
  await win.waitForTimeout(1700);
  await press('F9', () => waitVisible(editor.locator('.glyph-margin-widgets .codicon-debug-breakpoint')));
  await win.waitForTimeout(1650);
  await press('Control+Shift+d', () => waitVisible(configuration));
  await click(configuration);
  const launchOption = win.locator('.context-view').getByText(launchName, { exact: true });
  await waitVisible(launchOption);
  await win.waitForTimeout(1500);
  await click(launchOption);
  await waitVisible(selectedLaunch);
  await animMoveMouse({ x: 1120, y: 190 }, 300);
  await win.waitForTimeout(1500);

  await press('F5', async () => {
    await waitVisible(highlight, 15000);
    await waitVisible(variables.filter({ hasText: /\btotal\b.*\b7\b/ }));
  });
  await hold('breakpoint', 1700);

  await press('Control+Shift+y', async () => {
    await waitVisible(highlight);
    await waitVisible(consoleOutput.getByText(/Starting score: 1/));
  });
  await hold('console', 1700);

  await press('F5', async () => {
    await waitVisible(consoleOutput.getByText(/Points earned: 8/));
    await waitVisible(consoleOutput.getByText(/Total: 9/));
    await highlight.waitFor({ state: 'hidden', timeout: 5000 });
    await waitVisible(selectedLaunch);
    await ensurePortAvailable();
  });
  await hold('end', 1800);
  return samples;
}
