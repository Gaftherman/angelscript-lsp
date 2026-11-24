#!/usr/bin/env node

/**
 * @file record-demo.mjs
 * Reproducible demo recording tool for AngelScript LSP extension README demos.
 * Uses Playwright _electron + @vscode/test-electron to record VS Code scenarios,
 * and converts them via ffmpeg into crisp 960px animated GIFs under 3 MB.
 */

import { _electron as electron } from 'playwright-core';
import { downloadAndUnzipVSCode } from '@vscode/test-electron';
import path from 'path';
import fs from 'fs';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';
import {
  DEBUGGER_SCENARIO, DEBUGGER_WORKSPACE_DIR, copyDebuggerWorkspace, launchDebuggerHost,
  prepareDebuggerWindow, runDebuggerSteps, writeDebuggerCompatibilityExtension, writeDebuggerContactSheets,
  writeDebuggerReview,
} from './record-debugger.mjs';
import {
  DEBUGGER_ENGINE_SCENARIO, prepareDebuggerEngine, runDebuggerEngineSteps,
} from './record-debugger-engine.mjs';
import {
  DEBUGGER_LAUNCH_SCENARIO, buildDebuggerLaunchWorkspace, prepareDebuggerLaunch, runDebuggerLaunchSteps,
} from './record-debugger-launch.mjs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const CLIENT_ROOT = path.resolve(__dirname, '../..');
const WORKSPACE_DIR = path.resolve(__dirname, 'workspace');
const NEUTRAL_WORKSPACE_DIR = 'C:\\demo\\medkit';
const SCENARIOS_DIR = path.resolve(__dirname, 'scenarios');
const IMAGES_DIR = path.resolve(CLIENT_ROOT, 'images');
const VSCODE_TEST_DIR = path.resolve(CLIENT_ROOT, '.vscode-test');
const EMPTY_EXT_DIR = path.resolve(VSCODE_TEST_DIR, 'empty-extensions');
const TEMP_VIDEOS_DIR = path.resolve(VSCODE_TEST_DIR, 'temp-videos');
const REVIEW_DIR = path.join(TEMP_VIDEOS_DIR, 'review');
const USER_SETTINGS = JSON.parse(fs.readFileSync(path.join(__dirname, 'user-settings.json'), 'utf8'));

for (const tool of ['ffmpeg', 'ffprobe']) {
  try {
    execFileSync(tool, ['-version'], { stdio: 'pipe' });
  } catch {
    console.error(`Error: ${tool} is not found on PATH.`);
    process.exit(1);
  }
}

// Ensure output directories exist
fs.mkdirSync(IMAGES_DIR, { recursive: true });
fs.mkdirSync(EMPTY_EXT_DIR, { recursive: true });
fs.mkdirSync(REVIEW_DIR, { recursive: true });

// Parse CLI flags
const args = process.argv.slice(2);
let onlyFeatures = [];
let runAll = false;

for (let i = 0; i < args.length; i++) {
  if (args[i] === '--only' && args[i + 1]) {
    onlyFeatures.push(args[++i].replace(/\.json$/, ''));
  } else if (args[i] === '--all') {
    runAll = true;
  }
}

if (!runAll && onlyFeatures.length === 0) {
  onlyFeatures = ['hover'];
}

// Feature demos in recording order.
const ALL_FEATURES = [
  'hover',
  'autocomplete',
  'diagnostics',
  'engine-options',
  'engine-rules',
  'signature-help',
  'go-to-definition',
  'find-references',
  'rename',
  'code-actions',
  'formatting',
  'inlay-hints',
  'code-lens',
  'outline',
  'semantic-highlighting',
  'predefined-api'
];

const targetFeatures = runAll ? ALL_FEATURES : onlyFeatures;
const debuggerExample = path.resolve(CLIENT_ROOT, '../../angelscript-debug-example');
const debuggerScenarios = {
  debugger: DEBUGGER_SCENARIO,
  'debugger-engine': DEBUGGER_ENGINE_SCENARIO,
  'debugger-launch': DEBUGGER_LAUNCH_SCENARIO,
};

// Reserve the feature workspace only for runs that use it.
if (targetFeatures.some(feature => !Object.hasOwn(debuggerScenarios, feature))) {
  if (fs.existsSync(NEUTRAL_WORKSPACE_DIR)) {
    throw new Error('The neutral demo workspace is already in use.');
  }
  fs.mkdirSync(path.dirname(NEUTRAL_WORKSPACE_DIR), { recursive: true });
  fs.mkdirSync(NEUTRAL_WORKSPACE_DIR);
  process.on('exit', () => {
    try {
      if (fs.existsSync(NEUTRAL_WORKSPACE_DIR)) {
        fs.rmSync(NEUTRAL_WORKSPACE_DIR, { recursive: true, force: true });
      }
    } catch {}
  });
}

console.log('=== AngelScript LSP Demo Recorder ===');
console.log(`Targets (${targetFeatures.length}): ${targetFeatures.join(', ')}`);

function ffmpeg(args) {
  execFileSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: 'pipe' });
}

function probeDuration(file) {
  return Number(execFileSync('ffprobe', ['-v', 'error', '-show_entries', 'format=duration',
    '-of', 'default=noprint_wrappers=1:nokey=1', file], { encoding: 'utf8' }).trim());
}

async function runScenario(featureName) {
  const isDebugger = Object.hasOwn(debuggerScenarios, featureName);
  const isEngineDebugger = featureName === 'debugger-engine';
  const isLaunchDebugger = featureName === 'debugger-launch';
  const workspaceDir = isDebugger ? DEBUGGER_WORKSPACE_DIR : NEUTRAL_WORKSPACE_DIR;
  const scenarioFile = path.join(SCENARIOS_DIR, `${featureName}.json`);
  if (!isDebugger && !fs.existsSync(scenarioFile)) {
    console.warn(`Scenario file not found for ${featureName}.`);
    return null;
  }

  const scenario = isDebugger ? debuggerScenarios[featureName] : JSON.parse(fs.readFileSync(scenarioFile, 'utf8'));
  if (!isDebugger) fs.cpSync(WORKSPACE_DIR, workspaceDir, { recursive: true });
  if (scenario.setup?.storageRulesOnly) {
    const rulesPath = path.join(workspaceDir, scenario.setup.tab);
    const { storageRules } = JSON.parse(fs.readFileSync(rulesPath, 'utf8'));
    fs.writeFileSync(rulesPath, JSON.stringify({ storageRules }, null, 2) + '\n');
  }

  console.log(`\n--- Recording scenario: ${featureName} ---`);

  const vscodePath = await downloadAndUnzipVSCode('1.146.0');
  const targetFiles = [scenario.file, scenario.setup?.tab].filter(Boolean)
    .map(file => path.join(workspaceDir, file));
  const isSplit = scenario.setup?.action === 'splitRight';
  const isScreenshot = scenario.steps.some(step => step.action === 'screenshot');
  const cropHeight = scenario.cropHeight || 720;
  const zoomIn = scenario.zoomIn || scenario.initialZoom || 1.0;
  let lastTokenCoords = null;
  const zoomOrigin = scenario.zoomOrigin || '360px 420px';

  const userDataDir = fs.mkdtempSync(path.join(VSCODE_TEST_DIR, 'demo-user-data-'));
  fs.mkdirSync(path.join(userDataDir, 'User'));
  fs.writeFileSync(path.join(userDataDir, 'User', 'settings.json'), JSON.stringify({
    ...USER_SETTINGS,
    ...scenario.editorSettings
  }, null, 2));

  let app = null;
  let debuggerHost;
  let cleanDebuggerWorkspace;
  try {
    const extraExtensions = [];
    if (isDebugger) {
      cleanDebuggerWorkspace = copyDebuggerWorkspace(debuggerExample);
      if (isLaunchDebugger) {
        buildDebuggerLaunchWorkspace(debuggerExample, workspaceDir);
      } else {
        debuggerHost = launchDebuggerHost(debuggerExample);
        await debuggerHost.ready;
      }
      extraExtensions.push('--extensionDevelopmentPath=' + writeDebuggerCompatibilityExtension(userDataDir));
    }
    app = await electron.launch({
      executablePath: vscodePath,
      args: [
        '--window-size=1280,720',
        '--force-device-scale-factor=1',
        workspaceDir,
        '--extensionDevelopmentPath=' + CLIENT_ROOT,
        ...extraExtensions,
        '--extensions-dir=' + EMPTY_EXT_DIR,
        '--user-data-dir=' + userDataDir,
        '--locale=en',
        '--no-sandbox',
        '--disable-gpu-sandbox',
        '--disable-updates',
        '--disable-telemetry',
        '--disable-workspace-trust',
        '--skip-welcome',
        '--skip-release-notes',
        ...targetFiles
      ],
      recordVideo: isScreenshot ? undefined : {
        dir: TEMP_VIDEOS_DIR,
        size: { width: 1280, height: 720 }
      }
    });

  const win = await app.firstWindow();
  await win.waitForSelector('.monaco-workbench', { timeout: 15000 });
  try {
    const bw = await app.browserWindow(win);
    const bounds = await bw.evaluate((b) => {
      b.setContentSize(1280, 720);
      b.center();
      return b.getContentBounds();
    });
    console.log('BrowserWindow content bounds:', bounds);
  } catch (err) {
    console.error('BrowserWindow err:', err.message);
  }
  const dims = await win.evaluate(() => ({
    innerWidth: window.innerWidth,
    innerHeight: window.innerHeight,
    dpr: window.devicePixelRatio
  }));
  console.log('Window dimensions:', dims);
  try {
    await win.setViewportSize({ width: 1280, height: 720 });
  } catch {}

  if (!isDebugger) {
    // Close primary and secondary sidebars cleanly via VS Code command palette
    await win.keyboard.press('Control+Shift+P');
    await win.waitForTimeout(150);
    await win.keyboard.type('View: Close Primary Side Bar');
    await win.keyboard.press('Enter');
    await win.waitForTimeout(150);

    await win.keyboard.press('Control+Shift+P');
    await win.waitForTimeout(150);
    await win.keyboard.type('View: Close Secondary Side Bar');
    await win.keyboard.press('Enter');
    await win.waitForTimeout(200);

    // Close the real side bars so VS Code relayouts the editor at full width (CSS alone leaves stale layout widths)
    for (const _ of [0, 1]) {
      const sidebarOpen = await win.evaluate(() => {
        const el = document.querySelector('.part.sidebar');
        return !!el && el.offsetWidth > 0 && getComputedStyle(el).display !== 'none';
      });
      console.log('sidebarOpen=' + sidebarOpen);
      if (!sidebarOpen) break;
      await win.keyboard.press('Control+b');
      await win.waitForTimeout(400);
    }
    for (const _ of [0, 1]) {
      const auxOpen = await win.evaluate(() => { const e = document.querySelector('.part.auxiliarybar'); return !!e && e.getBoundingClientRect().width > 10; });
      if (!auxOpen) break;
      await win.keyboard.press('Control+Alt+b');
      await win.waitForTimeout(500);
    }
    console.log('layout=' + JSON.stringify(await win.evaluate(() => ['.part.sidebar','.part.auxiliarybar','.part.activitybar','.part.editor'].map(q => { const e=document.querySelector(q); return e? q+':'+Math.round(e.getBoundingClientRect().width)+'@'+getComputedStyle(e).display : q+':none'; }))));
  }
  // Setup pristine UI layout filling 1280x720 without any black borders
  await win.evaluate(({ initZoom, origin, hideHover, isSplit, isSnap, suggestWidth, wholeLineMargin, cropHeight, actionMenuFontSize, isDebugger }) => {
    const style = document.createElement('style');
    style.textContent = `${isDebugger ? '' : `
      .part.auxiliarybar, .part.sidebar, .notifications-toasts, .part.statusbar, .part.titlebar, .part.activitybar {
        display: none !important;
      }
      .part.editor .title.tabs {
        ${isSplit ? 'display: flex !important;' : 'display: none !important;'}
      }
      .part.editor .tab {
        font-size: 16px !important;
        height: 38px !important;
      }
      .monaco-workbench {
        position: fixed !important;
        top: 0 !important;
        left: 0 !important;
        width: 1280px !important;
        height: 720px !important;
        overflow: hidden !important;
      }
      .part.editor {
        position: fixed !important;
        top: 0 !important;
        left: 0 !important;
        width: 1280px !important;
        height: 720px !important;
      }
      ${isSplit ? '' : `
        .monaco-editor, .monaco-editor .overflow-guard { width: 1280px !important; }
      `}
      /* Tooltip width and margin protection */
      .monaco-hover {
        max-width: ${isSplit ? 520 : 720}px !important;
        ${hideHover ? 'display: none !important;' : ''}
      }
      ${isSplit ? `
        .monaco-hover, .monaco-hover .monaco-scrollable-element, .monaco-hover-content {
          max-height: 440px !important;
        }
        .monaco-hover .hover-row {
          font-size: 17px;
        }
      ` : ''}
      /* Ensure quick-input search box never touches top edge */
      .quick-input-widget {
        top: 36px !important;
        max-height: 480px !important;
        width: 760px !important;
      }
      .quick-input-list .monaco-list-row {
        font-size: 17px !important;
      }
      .quick-input-box input {
        font-size: 19px !important;
      }
      /* Code actions / quick fix menu */
      .action-widget {
        font-size: 18px !important;
      }
      .action-widget .monaco-list-row {
        font-size: 17px !important;
      }
      ${actionMenuFontSize ? `
        .context-view .action-widget .monaco-list-row[role="option"] .title {
          font-size: ${actionMenuFontSize}px;
        }
      ` : ''}
      /* Peek widget (code lens / find references) */
      .zone-widget, .zone-widget-container, .peekview-widget {
        width: 1236px !important;
      }
      .peekview-widget .body .split-view-view:nth-child(1) { left: 0px !important; width: 600px !important; }
      .peekview-widget .body .split-view-view:nth-child(2) { left: 604px !important; width: 624px !important; }
      .peekview-widget .preview {
        left: 0 !important;
        width: 600px !important;
        min-width: 600px !important;
      }
      .peekview-widget .ref-tree,
      .peekview-widget .tree,
      .peekview-widget .results {
        left: 0px !important;
        width: 624px !important;
        min-width: 624px !important;
      }
      .peekview-widget .ref-tree .monaco-list,
      .peekview-widget .ref-tree .monaco-list-rows,
      .peekview-widget .ref-tree .monaco-list-row {
        width: 624px !important;
      }
      .peekview-widget .ref-tree,
      .peekview-widget .ref-tree * {
        font-size: 15px !important;
        max-width: none !important;
        overflow: visible !important;
      }
      .peekview-widget .ref-tree .monaco-list-row {
        height: 30px !important;
        line-height: 30px !important;
      }
      .codelens-decoration {
        font-size: 20px !important;
        font-weight: 600 !important;
      }
      /* Inlay hints contrast & styling */
      .inlay-hint {
        font-size: 20px !important;
        font-weight: 600 !important;
      }
      /* Widen autocomplete / suggest widget to avoid ellipsized signatures */
      .suggest-widget {
        width: ${suggestWidth}px !important;
        max-width: ${suggestWidth}px !important;
      }
      .suggest-widget .monaco-list,
      .suggest-widget .monaco-list-rows,
      .suggest-widget .monaco-list-row {
        width: ${suggestWidth}px !important;
        max-width: ${suggestWidth}px !important;
      }
      .suggest-widget .monaco-list-row .contents {
        width: ${suggestWidth - 20}px !important;
        max-width: ${suggestWidth - 20}px !important;
      }
      .suggest-widget .monaco-list-row .contents .main {
        max-width: ${suggestWidth - 40}px !important;
      }
      .suggest-widget .monaco-list-row .contents .main .signature-label,
      .suggest-widget .monaco-list-row .contents .main .monaco-icon-label {
        overflow: visible !important;
        text-overflow: clip !important;
        max-width: none !important;
      }
      /* Rename input box */
      .rename-box {
        max-width: 480px !important;
        width: 460px !important;
      }
      .rename-box .rename-input {
        width: 460px !important;
        max-width: 460px !important;
      }
    `}
      .demo-edge-row {
        visibility: hidden;
      }
      #demo-keycaps {
        position: fixed;
        right: 32px;
        display: flex;
        gap: 8px;
        z-index: 2147483646;
        pointer-events: none;
        --first-press: 160ms;
        --key-gap: 250ms;
        --press-duration: 180ms;
      }
      #demo-keycaps kbd {
        position: relative;
        min-width: 40px;
        padding: 7px 12px;
        border: 1px solid #737881;
        border-bottom-width: 3px;
        border-radius: 7px;
        background: #34373c;
        color: #c4c8ce;
        text-align: center;
        font: 600 22px/1.2 system-ui, sans-serif;
        animation: demo-keycap-lit 0s both;
        animation-delay: var(--press-time);
        --press-time: calc(var(--first-press) + (var(--key-index) - 1) * var(--key-gap));
      }
      @supports (animation-delay: calc(sibling-index() * 1ms)) {
        #demo-keycaps kbd { --key-index: sibling-index(); }
      }
      #demo-keycaps kbd::after {
        content: '';
        position: absolute;
        inset: -1px;
        border: 2px solid #8dccff;
        border-radius: inherit;
        opacity: 0;
      }
      @media (prefers-reduced-motion: no-preference) {
        #demo-keycaps kbd {
          animation: demo-keycap-lit 0s var(--press-time) both,
            demo-keycap-press var(--press-duration) ease-out var(--press-time) both;
        }
        #demo-keycaps kbd::after {
          animation: demo-keycap-flash var(--press-duration) ease-out var(--press-time) forwards;
        }
      }
      @keyframes demo-keycap-lit {
        to { background: #244767; color: #fff; border-color: #8dccff; }
      }
      @keyframes demo-keycap-press {
        0%, 100% { transform: scale(1); }
        35% { transform: scale(.9); }
      }
      @keyframes demo-keycap-flash {
        from { opacity: 1; }
        to { opacity: 0; }
      }
      #demo-keycaps.demo-keycaps-fade {
        animation: demo-keycaps-fade 200ms linear var(--demo-keycap-hold, 1000ms) forwards;
      }
      @keyframes demo-keycaps-fade {
        to { opacity: 0; }
      }
    `;
    document.head.appendChild(style);

    // Set initial zoom level on workbench
    const wb = document.querySelector('.monaco-workbench');
    if (wb && initZoom !== 1) {
      wb.style.transformOrigin = origin;
      wb.style.transform = `scale(${initZoom})`;
    }

    if (isSnap) {
      // Hide partial text lines at the editor top/bottom so only whole lines are ever visible.
      const snap = () => {
        const ed = document.querySelector('.part.editor .editor-container .monaco-editor');
        if (ed) {
          const r = ed.getBoundingClientRect();
          const tops = Array.from(ed.querySelectorAll('.overflow-guard > .monaco-scrollable-element > .lines-content > .view-lines > .view-line')).map(l => l.getBoundingClientRect()).filter(b => b.height > 0 && b.bottom > r.top && b.top < r.bottom);
          if (tops.length) {
            const lh = tops[0].height;
            let first = Math.min(...tops.map(b => b.top));
            let o = first - r.top; while (o < 0) o += lh;
            const whole = Math.floor((r.height - o) / lh) * lh;
            const bottomGap = Math.max(0, r.height - o - whole);
            window.__snapDbg = {o, lh, whole, bottomGap, rtop: r.top, rh: r.height, n: tops.length, tops: tops.map(b => Math.round(b.top)).slice(0,30)};
            ed.style.clipPath = 'inset(' + Math.round(o) + 'px 0 ' + Math.round(bottomGap) + 'px 0)';
          }
        }
        requestAnimationFrame(snap);
      };
      requestAnimationFrame(snap);
    }

    if (wholeLineMargin) {
      // Hide edge rows individually. Monaco's scroll surface and widgets keep their own geometry.
      const frameLines = () => {
        for (const container of document.querySelectorAll('.part.editor .editor-container')) {
          const editor = container.querySelector('.monaco-editor');
          if (!editor || !editor.clientHeight) continue;
          const bounds = editor.getBoundingClientRect();
          const top = Math.max(0, bounds.top) + wholeLineMargin;
          const bottom = Math.min(cropHeight, bounds.bottom) - wholeLineMargin;
          const rows = editor.querySelectorAll('.view-lines > .view-line, .line-numbers, .codelens-decoration');
          const edges = Array.from(rows, row => {
            const bounds = row.getBoundingClientRect();
            return bounds.top < top || bounds.bottom > bottom;
          });
          rows.forEach((row, index) => row.classList.toggle('demo-edge-row', edges[index]));
        }
        requestAnimationFrame(frameLines);
      };
      requestAnimationFrame(frameLines);
    }
    window.dispatchEvent(new Event('resize'));
  }, {
    initZoom: zoomIn,
    origin: zoomOrigin,
    hideHover: ['semantic-highlighting', 'autocomplete', 'predefined-api'].includes(featureName),
    isSplit,
    isSnap: featureName === 'code-lens',
    suggestWidth: scenario.suggestWidth || 720,
    wholeLineMargin: scenario.wholeLineMargin,
    cropHeight,
    actionMenuFontSize: scenario.actionMenuFontSize,
    isDebugger,
  });

  // Ensure an editor with content is loaded
  const hasContent = await win.evaluate(() => {
    return document.querySelectorAll('.view-lines span').length > 5;
  });
  if (!hasContent && scenario.file) {
    console.log(`Editor not active, opening ${scenario.file} via Quick Open...`);
    await win.keyboard.press('Control+p');
    await win.waitForTimeout(300);
    await win.keyboard.type(scenario.file, { delay: 40 });
    await win.waitForTimeout(400);
    await win.keyboard.press('Enter');
    await win.waitForTimeout(1000);
  }

  // Initial navigation before starting recording clock
  if (scenario.preLine) {
    await win.keyboard.press('Control+g');
    await win.waitForTimeout(150);
    await win.keyboard.type(String(scenario.preLine));
    await win.keyboard.press('Enter');
    await win.waitForTimeout(300);
  }
  if (scenario.initialLine) {
    await win.keyboard.press('Control+g');
    await win.waitForTimeout(150);
    await win.keyboard.type(String(scenario.initialLine));
    await win.keyboard.press('Enter');
    await win.waitForTimeout(400);
  }

  // Inject Trusted-Types compliant synthetic cursor inside document.body (viewport-fixed)
  await win.evaluate(() => {
    const cursor = document.createElement('div');
    cursor.id = 'demo-cursor';

    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('width', '24');
    svg.setAttribute('height', '24');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.style.filter = 'drop-shadow(0 2px 4px rgba(0,0,0,0.6))';

    const pathEl = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    pathEl.setAttribute('d', 'M5.5 3.2L18.5 13.8L12.5 14.5L15.8 21.2L12.7 22.6L9.5 16.0L5.5 19.5V3.2Z');
    pathEl.setAttribute('fill', '#FFFFFF');
    pathEl.setAttribute('stroke', '#1E1E1E');
    pathEl.setAttribute('stroke-width', '1.5');
    pathEl.setAttribute('stroke-linejoin', 'round');
    svg.appendChild(pathEl);
    cursor.appendChild(svg);

    const ripple = document.createElement('div');
    ripple.id = 'demo-cursor-ripple';
    ripple.style.cssText = 'position: absolute; top: 0px; left: 0px; width: 32px; height: 32px; margin-left: -12px; margin-top: -12px; border-radius: 50%; border: 2.5px solid #007ACC; background: rgba(0, 122, 204, 0.25); transform: scale(0); opacity: 0; pointer-events: none; transition: none;';
    cursor.appendChild(ripple);

    cursor.style.cssText = `
      position: fixed;
      top: 0;
      left: 0;
      pointer-events: none;
      z-index: 2147483647;
      transform: translate(650px, 200px);
      will-change: transform;
    `;
    document.body.appendChild(cursor);

    window.__cursorX = 650;
    window.__cursorY = 200;

    window.__moveCursor = function(targetX, targetY, durationMs = 600) {
      return new Promise((resolve) => {
        const startX = window.__cursorX;
        const startY = window.__cursorY;
        const startTime = performance.now();

        function easeInOutCubic(t) {
          return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
        }

        function frame(now) {
          const elapsed = now - startTime;
          const progress = Math.min(elapsed / durationMs, 1);
          const eased = easeInOutCubic(progress);

          const curX = startX + (targetX - startX) * eased;
          const curY = startY + (targetY - startY) * eased;

          cursor.style.transform = `translate(${curX}px, ${curY}px)`;

          if (progress < 1) {
            requestAnimationFrame(frame);
          } else {
            window.__cursorX = targetX;
            window.__cursorY = targetY;
            resolve();
          }
        }

        requestAnimationFrame(frame);
      });
    };

    window.__clickRipple = function() {
      const rip = document.getElementById('demo-cursor-ripple');
      if (!rip) return;
      rip.style.transition = 'none';
      rip.style.transform = 'scale(0)';
      rip.style.opacity = '1';
      void rip.offsetWidth;
      rip.style.transition = 'transform 320ms cubic-bezier(0, 0, 0.2, 1), opacity 320ms cubic-bezier(0, 0, 0.2, 1)';
      rip.style.transform = 'scale(1.5)';
      rip.style.opacity = '0';
    };
  });

  // Wait for LSP server to index and publish diagnostics/tokens
  await win.waitForTimeout(2000);

  const findTokenCoords = async ({ toToken, lineIncludes, lineOffset = 0 }) => {
    return win.evaluate(({ token, line, offset }) => {
      const elements = Array.from(document.querySelectorAll('.view-lines span, .codelens-decoration a, .codelens-decoration span, .tab-label a, .monaco-list-row span, a'));
      const visible = elements.flatMap(element => {
        const row = element.closest('.view-line');
        if (row?.classList.contains('demo-edge-row')) return [];
        if (line && !row?.textContent.replace(/\u00a0/g, ' ').includes(line)) return [];
        const text = element.textContent.trim();
        if (!text.includes(token)) return [];
        const bounds = element.getBoundingClientRect();
        const y = bounds.top + bounds.height / 2 + (row?.getBoundingClientRect().height || 0) * offset;
        const x = bounds.left + bounds.width / 2;
        return bounds.width > 0 && x >= 0 && x < innerWidth && y >= 0 && y < innerHeight
          ? [{ x, y, exact: text === token, width: bounds.width }]
          : [];
      });
      visible.sort((a, b) => Number(b.exact) - Number(a.exact) || a.width - b.width ||
        Math.abs(a.y - innerHeight / 2) - Math.abs(b.y - innerHeight / 2));
      return visible[0] ?? null;
    }, { token: toToken, line: lineIncludes, offset: lineOffset });
  };

  const animMoveMouse = async (coords, durationMs = 600) => {
    const targetX = coords.x;
    const targetY = coords.y;

    const curPos = await win.evaluate(() => ({ x: window.__cursorX, y: window.__cursorY }));
    if (curPos.x === targetX && curPos.y === targetY) return;
    const movePromise = win.evaluate(({ tx, ty, ms }) => window.__moveCursor(tx, ty, ms), { tx: targetX, ty: targetY, ms: durationMs });
    const steps = Math.min(16, Math.max(1, Math.ceil(durationMs / 25)));
    for (let i = 1; i <= steps; i++) {
      const t = i / steps;
      const eased = t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
      const cx = curPos.x + (targetX - curPos.x) * eased;
      const cy = curPos.y + (targetY - curPos.y) * eased;
      await win.mouse.move(cx, cy);
      await win.waitForTimeout(durationMs / steps);
    }
    await movePromise;
    await win.mouse.move(targetX, targetY);
  };

  const animClick = async (coords, clickCount = 1) => {
    const curPos = await win.evaluate(() => ({ x: window.__cursorX, y: window.__cursorY }));
    const targetX = coords ? (coords.x !== undefined ? coords.x : coords.vx) : curPos.x;
    const targetY = coords ? (coords.y !== undefined ? coords.y : coords.vy) : curPos.y;
    if (coords) {
      await animMoveMouse(coords, 400);
    }
    await win.evaluate(() => window.__clickRipple());
    await win.mouse.click(targetX, targetY, { clickCount });
  };

  const clickTab = async (tabName) => {
    const name = path.basename(tabName);
    const tab = win.locator('.part.editor .tab').filter({ has: win.getByText(name, { exact: true }) });
    await tab.click();
  };

  const keyLabels = keys => keys.split('+').map(key => ({
    control: 'Ctrl', ctrl: 'Ctrl', meta: 'Win', win: 'Win',
    shift: 'Shift', alt: 'Alt', space: 'Space',
  }[key.toLowerCase()] ?? key.toUpperCase()));

  const showKeys = async (labels) => {
    const pressSequence = await win.evaluate(({ labels, bottom, holdMs, gapMs }) => {
      document.querySelector('#demo-keycaps')?.remove();
      const overlay = document.createElement('div');
      overlay.id = 'demo-keycaps';
      overlay.setAttribute('aria-hidden', 'true');
      overlay.style.bottom = `${bottom}px`;
      if (holdMs !== undefined) overlay.style.setProperty('--demo-keycap-hold', `${holdMs}ms`);
      if (gapMs !== undefined) overlay.style.setProperty('--key-gap', `${gapMs}ms`);
      const hasSiblingIndex = CSS.supports('animation-delay: calc(sibling-index() * 1ms)');
      for (const [index, label] of labels.entries()) {
        const cap = document.createElement('kbd');
        cap.textContent = label;
        if (!hasSiblingIndex) cap.style.setProperty('--key-index', String(index + 1));
        overlay.appendChild(cap);
      }
      overlay.addEventListener('animationend', event => {
        if (event.animationName === 'demo-keycaps-fade') overlay.remove();
      });
      document.body.appendChild(overlay);
      const style = getComputedStyle(overlay);
      const firstPress = parseFloat(style.getPropertyValue('--first-press'));
      const gap = parseFloat(style.getPropertyValue('--key-gap'));
      const duration = parseFloat(style.getPropertyValue('--press-duration'));
      return firstPress + (labels.length - 1) * gap + duration;
    }, { labels, bottom: 720 - cropHeight + 32, holdMs: scenario.keycapHoldMs, gapMs: scenario.keycapGapMs });
    await win.waitForTimeout(pressSequence + 500);
  };

  const fadeKeys = () => win.evaluate(() => {
    document.querySelector('#demo-keycaps')?.classList.add('demo-keycaps-fade');
  });

  let preShownKeys;
  const press = async (key, waitForResult) => {
    const labels = keyLabels(key);
    const shown = labels.join('+') === preShownKeys;
    const hasOverlay = shown || key.includes('+') || /^F(?:[1-9]|1[0-2])$/i.test(key);
    if (hasOverlay && !shown) await showKeys(labels);
    preShownKeys = undefined;
    await win.keyboard.press(key);
    if (waitForResult) await waitForResult();
    if (hasOverlay) await fadeKeys();
  };

  const expectEditorText = async (expected) => {
    await win.waitForFunction(text => Array.from(document.querySelectorAll('.part.editor .view-line'))
      .some(line => line.textContent.replace(/\u00a0/g, ' ').includes(text)), expected, { timeout: 2500 })
      .catch(() => { throw new Error('Expected visible editor text: ' + expected); });
  };

  if (isDebugger) {
    if (isEngineDebugger) await clickTab(scenario.file);
    if (isLaunchDebugger) await prepareDebuggerLaunch(win, workspaceDir);
    else await prepareDebuggerWindow(win, scenario.sidebarWidth);
    if (isEngineDebugger) await prepareDebuggerEngine(win);
    await animMoveMouse({ x: 1140, y: 420 });
    // Leave stable footage before the scene trim to cover window-close latency.
    await win.waitForTimeout(1600);
  }

  if (isSplit) {
    await clickTab(scenario.setup.tab);
    await win.keyboard.press('Control+Shift+P');
    await win.keyboard.type('View: Move Editor into Right Group');
    await win.locator('.quick-input-widget .monaco-list-row.focused').filter({ hasText: 'Move Editor into Right Group' }).waitFor();
    await win.keyboard.press('Enter');
    await win.locator('.part.editor .editor-group-container').nth(1).waitFor();
    const sash = win.locator('.part.editor .monaco-grid-view .monaco-sash.vertical:visible').first();
    const divider = await sash.boundingBox();
    if (!divider) throw new Error('Could not find the editor split divider.');
    const y = divider.y + divider.height / 2;
    await win.mouse.move(divider.x + divider.width / 2, y);
    await win.mouse.down();
    await win.mouse.move(1280 * 0.7, y, { steps: 8 });
    await win.mouse.up();
    const right = await win.locator('.part.editor .editor-group-container').nth(1).boundingBox();
    if (!right || Math.abs(right.x - 1280 * 0.7) > 25) {
      throw new Error('Could not size the editor split to 70/30.');
    }
    await clickTab(scenario.file);
    await win.keyboard.press('Control+Home');
    await win.waitForTimeout(450);
  }

  // Mark first scenario action timestamp
  const firstActionTime = Date.now();
  const runDebugger = { debugger: runDebuggerSteps, 'debugger-engine': runDebuggerEngineSteps,
    'debugger-launch': runDebuggerLaunchSteps }[featureName];
  const debuggerReview = isDebugger
    ? await runDebugger({ win, animClick, animMoveMouse, press, firstActionTime }) : null;

  for (const [stepIndex, step] of scenario.steps.entries()) {
    if (step.action === 'goto') {
      await win.keyboard.press('Control+g');
      await win.waitForTimeout(150);
      await win.keyboard.type(step.target.split(':')[0], { delay: 30 });
      await win.keyboard.press('Enter');
      await win.waitForTimeout(400);
    } else if (step.action === 'scrollLines') {
      const count = step.count || 5;
      for (let i = 0; i < count; i++) {
        await win.keyboard.press('Control+ArrowDown');
        await win.waitForTimeout(40);
      }
    } else if (step.action === 'ensureHover') {
      let matchingHover = win.locator('.monaco-hover:visible .monaco-hover-content');
      for (const text of [step.expect || /\S/].flat()) {
        matchingHover = matchingHover.filter({ hasText: text });
      }
      const hover = matchingHover.first();
      await hover.waitFor({ timeout: 3500 });
      if (step.above) {
        const bounds = await hover.boundingBox();
        const cursorY = await win.evaluate(() => window.__cursorY);
        if (bounds.y + bounds.height > cursorY) throw new Error('Hover appeared below the target.');
      }
      if (step.fullyVisible) {
        const fits = await hover.evaluate((content, crop) => {
          const bounds = content.getBoundingClientRect();
          return content.scrollHeight <= content.clientHeight && content.scrollWidth <= content.clientWidth &&
            bounds.top >= 0 && bounds.bottom <= crop && bounds.left >= 0 && bounds.right <= innerWidth;
        }, cropHeight);
        if (!fits) throw new Error('Hover content extends beyond the recording or its scroll area.');
      }
      if (process.env.DEMO_DEBUG) console.log('Hover:', (await hover.innerText()).trim());
    } else if (step.action === 'moveCursor') {
      const coords = step.toToken ? await findTokenCoords(step) : { x: step.x, y: step.y };
      if (!coords) throw new Error('Could not locate token ' + step.toToken);
      if (step.toToken) lastTokenCoords = coords;
      if (step.toLastToken && lastTokenCoords) {
        const ratio = (scenario.zoomOut || 1.0) / zoomIn;
        coords.x = lastTokenCoords.x * ratio;
        coords.y = lastTokenCoords.y * ratio;
      }
      await animMoveMouse(coords, step.duration || 650);
    } else if (step.action === 'click') {
      const coords = step.toToken ? await findTokenCoords(step) :
        (step.x === undefined ? null : { x: step.x, y: step.y });
      if (step.toToken && !coords) throw new Error('Could not locate token ' + step.toToken);
      await animClick(coords, step.clickCount);
    } else if (step.action === 'smoothScroll') {
      const bounds = await win.locator('.part.editor .editor-group-container.active .monaco-editor').first().boundingBox();
      await animMoveMouse({ x: bounds.x + bounds.width * 0.8, y: bounds.y + bounds.height * 0.65 }, 260);
      let target;
      for (let i = 0; i < 28; i++) {
        target = await findTokenCoords(step);
        const delta = target ? Math.round(target.y - step.targetY) : 110;
        if (Math.abs(delta) <= 35) break;
        await win.mouse.wheel(0, Math.max(-110, Math.min(delta, 110)));
        await win.waitForTimeout(80);
      }
      await win.waitForTimeout(180);
      target = await findTokenCoords(step);
      if (!target || Math.abs(target.y - step.targetY) > 60) {
        throw new Error('Scroll did not frame ' + step.toToken);
      }
    } else if (step.action === 'type') {
      await win.keyboard.type(step.text, { delay: step.delay ?? 75 });
    } else if (step.action === 'press') {
      await press(step.key);
    } else if (step.action === 'showKeys') {
      const labels = keyLabels(step.keys);
      await showKeys(labels);
      const next = scenario.steps[stepIndex + 1];
      if (next?.action === 'press' && labels.join('+') === keyLabels(next.key).join('+')) {
        preShownKeys = labels.join('+');
      } else {
        await fadeKeys();
      }
    } else if (step.action === 'expectEditorText') {
      await expectEditorText(step.text);
    } else if (step.action === 'ensureSignature') {
      await win.locator('.parameter-hints-widget:visible .parameter.active')
        .filter({ hasText: step.parameter }).first().waitFor({ timeout: 1800 });
    } else if (step.action === 'selectSuggestion') {
      const focused = win.locator('.suggest-widget.visible .monaco-list-row.focused .label-name').first();
      await focused.waitFor({ timeout: 2500 });
      for (let i = 0; i < 16 && (await focused.innerText()).trim() !== step.label; i++) {
        await win.keyboard.press('ArrowDown');
        await win.waitForTimeout(140);
      }
      if ((await focused.innerText()).trim() !== step.label) throw new Error('Could not highlight ' + step.label);
    } else if (step.action === 'cycleCodeActions') {
      const actions = win.locator('.action-widget:visible .monaco-list-row[role="option"]');
      const editorRows = '.part.editor .editor-instance .view-lines > .view-line';
      const readCode = rows => rows.map(line => line.textContent.replace(/\u00a0/g, ' ').trim()).join('\n');
      const focused = win.locator('.action-widget .monaco-list-row.focused').first();
      for (const [index, pick] of step.picks.entries()) {
        if (index > 0) {
          const target = pick.target ?? step;
          const coords = await findTokenCoords(target);
          if (!coords) throw new Error('Could not locate action target ' + target.toToken);
          await animMoveMouse(coords, 120);
          await animClick();
        }
        await press('Control+.');
        await actions.first().waitFor({ timeout: 2500 });
        const titles = (await actions.allTextContents()).map(title => title.trim());
        if (index === 0) {
          const expected = step.picks.map(item => item.title);
          if (titles.length !== expected.length || !expected.every(title => titles.includes(title))) {
            throw new Error('The opening menu differs from the recorded actions: ' + titles.join(' | '));
          }
          console.log('Opening code action menu:', titles.join(' | '));
        }
        const clippedTitle = await actions.evaluateAll((rows, height) => {
          for (const row of rows) {
            const label = row.querySelector('.title');
            const { top, right, bottom, left } = row.getBoundingClientRect();
            if (label.scrollWidth > label.clientWidth || top < 0 || right > innerWidth || bottom > height || left < 0) {
              return label.textContent;
            }
          }
        }, cropHeight);
        if (clippedTitle) throw new Error('Code action label clipped: ' + clippedTitle);
        if (!titles.includes(pick.title)) throw new Error('Could not find ' + pick.title + ' in ' + titles.join(' | '));
        if (!(await focused.count())) await win.keyboard.press('ArrowDown');
        for (let i = 0; i < titles.length && (await focused.textContent()).trim() !== pick.title; i++) {
          await win.keyboard.press('ArrowDown');
        }
        if ((await focused.textContent()).trim() !== pick.title) throw new Error('Could not highlight ' + pick.title);
        await win.waitForTimeout(step.menuHoldMs);
        const before = await win.locator(editorRows).evaluateAll(readCode);
        await win.keyboard.press('Enter');
        await actions.first().waitFor({ state: 'hidden', timeout: 1500 });
        if (pick.removes) {
          await win.locator(editorRows).filter({ hasText: pick.removes }).first().waitFor({ state: 'hidden', timeout: 2500 });
        }
        for (const line of pick.shows ?? []) await expectEditorText(line);
        if (await win.locator(editorRows).evaluateAll(readCode) === before) {
          throw new Error('No visible edit after applying ' + pick.title);
        }
        console.log('Applied code action at ' + ((Date.now() - firstActionTime) / 1000).toFixed(2) + ' s:', pick.title);
        await win.waitForTimeout(step.resultHoldMs);
        await win.keyboard.press('Control+z');
        await win.waitForFunction(({ rows, before }) => Array.from(document.querySelectorAll(rows),
          line => line.textContent.replace(/\u00a0/g, ' ').trim()).join('\n') === before,
          { rows: editorRows, before }, { timeout: 2500 })
          .catch(async () => { throw new Error('Undo for ' + pick.title + ' produced:\n' +
            await win.locator(editorRows).evaluateAll(readCode)); });
      }
    } else if (step.action === 'screenshot') {
      await win.screenshot({
        path: path.join(IMAGES_DIR, featureName + '.png'),
        clip: { x: 0, y: 0, width: 1280, height: cropHeight },
      });
      return { name: featureName, width: 1280 };
    } else if (step.action === 'wait') {
      await win.waitForTimeout(step.ms || 500);
    } else if (step.action === 'smoothZoomOut') {
      const zoomOut = scenario.zoomOut || 1.0;
      const dur = step.duration || 600;
      await win.evaluate(({ to, duration }) => {
        const wb = document.querySelector('.monaco-workbench');
        if (wb) {
          wb.style.transition = `transform ${duration}ms cubic-bezier(0.25, 1, 0.5, 1)`;
          wb.style.transform = `scale(${to})`;
        }
      }, { to: zoomOut, duration: dur });
      await win.waitForTimeout(dur);
    } else if (step.action === 'ensureDefinition') {
      await expectEditorText(step.expect);
    } else if (step.action === 'hold') {
      if (process.env.DEMO_DEBUG) console.log('GEOM ' + JSON.stringify(await win.evaluate(() => {
        const r = (q) => Array.from(document.querySelectorAll(q)).slice(0, 3).map(e => { const b = e.getBoundingClientRect(); return [Math.round(b.left), Math.round(b.top), Math.round(b.width), Math.round(b.height), e.scrollWidth]; });
        return { suggest: r('.suggest-widget'), row: r('.suggest-widget .monaco-list-row.focused'), main: r('.suggest-widget .monaco-list-row.focused .main'), peek: r('.peekview-widget'), tree: r('.peekview-widget .ref-tree'), rows: r('.peekview-widget .ref-tree .monaco-list-row'), prev: r('.peekview-widget .preview'), pl: r('.peekview-widget .preview .view-line'), ml: r('.monaco-editor .view-line'), bodyH: r('.peekview-widget .body'), hdr: r('.peekview-widget .head'), ed: r('.part.editor .monaco-editor'), tt: r('.part.editor .title'), og: r('.part.editor .overflow-guard'), clip: Array.from(document.querySelectorAll('.part.editor .monaco-editor')).map(e => e.style.clipPath + '|' + e.className.slice(0,40)), vl: r('.part.editor .view-line'), dbg: window.__snapDbg, chain: (() => { const out=[]; let e=document.querySelector('.part.editor .view-lines'); while(e && out.length<14){ const b=e.getBoundingClientRect(); out.push(String(e.className).slice(0,28)+':'+Math.round(b.top)+'+'+Math.round(b.height)+(getComputedStyle(e).overflow!=='visible'?'(ov)':'')); e=e.parentElement;} return out; })(), pts: [300,660,685,700,715].map(y => [y, document.elementsFromPoint(300,y).slice(0,4).map(e => e.tagName + '.' + String(e.className).slice(0,50))]), win: [window.innerWidth, window.innerHeight, document.documentElement.clientHeight, window.outerHeight, getComputedStyle(document.body).height] };
      })));
      await win.waitForTimeout(step.ms || 1500);
    }
  }

  const minimumDuration = isEngineDebugger || isLaunchDebugger ? 15.1 : isDebugger ? 20.1 : 6.1;
  const elapsed = (Date.now() - firstActionTime) / 1000;
  await win.waitForTimeout(Math.max(650, (minimumDuration - elapsed) * 1000));
  const sceneDuration = (Date.now() - firstActionTime) / 1000;
  const maximumDuration = { debugger: 30, 'debugger-engine': 25, 'debugger-launch': 25,
    'code-actions': 38, 'signature-help': 11 }[featureName] ?? 10;
  if (sceneDuration > maximumDuration) {
    throw new Error('Scenario took ' + sceneDuration.toFixed(2) + ' s (limit ' + maximumDuration + ' s).');
  }

  const video = win.video();
  const closeStarted = Date.now();
  await app.close();
  app = null;
  const closeDuration = (Date.now() - closeStarted) / 1000;
  const videoPath = await video.path();

  // Align to the recorded end. Exclude the window-close tail for the F9 overlay at launch.
  const trimStart = Math.max(0, probeDuration(videoPath) - sceneDuration - (isLaunchDebugger ? closeDuration : 0));
  const outputDir = isDebugger ? path.join(debuggerExample, 'media') : IMAGES_DIR;
  const gifOut = path.join(outputDir, featureName + '.gif');
  const reviewGif = path.join(REVIEW_DIR, featureName + '.gif');
  const palette = path.join(TEMP_VIDEOS_DIR, featureName + '-palette.png');
  const candidates = isDebugger ? [
    { width: 960, fps: 12, colors: 128, dither: 'none' },
    { width: 960, fps: 10, colors: 96, dither: 'none' },
    { width: 960, fps: 8, colors: 64, dither: 'none' },
    { width: 960, fps: 8, colors: 48, dither: 'none' },
  ] : featureName === 'code-actions' ? [
    { width: 960, fps: 10, colors: 96, dither: 'none' },
    { width: 960, fps: 10, colors: 64, dither: 'none' },
    { width: 960, fps: 8, colors: 64, dither: 'none' },
    { width: 880, fps: 10, colors: 64, dither: 'none' },
    { width: 880, fps: 8, colors: 48, dither: 'none' },
  ] : [
    { width: 960, fps: 15, colors: 128 },
    { width: 960, fps: 12, colors: 96 },
    { width: 960, fps: 12, colors: 64, dither: 'none' },
    { width: 960, fps: 10, colors: 64, dither: 'none' },
    { width: 800, fps: 12, colors: 64 },
    { width: 768, fps: 10, colors: 64 },
  ];

  const sizeLimit = isDebugger ? 5_000_000 : 3_000_000;
  let size;
  for (const { width, fps, colors, dither = 'bayer:bayer_scale=3' } of candidates) {
    const input = ['-ss', trimStart.toFixed(3), '-t', Math.min(sceneDuration, maximumDuration - 0.03).toFixed(3), '-i', videoPath];
    const filter = 'crop=1280:' + cropHeight + ':0:0,scale=' + width + ':-2:flags=lanczos,fps=' + fps;
    ffmpeg([...input, '-vf', filter + ',palettegen=stats_mode=diff:max_colors=' + colors, '-frames:v', '1', '-update', '1', palette]);
    ffmpeg([...input, '-i', palette, '-filter_complex',
      '[0:v]' + filter + '[scene];[scene][1:v]paletteuse=dither=' + dither + ':diff_mode=rectangle',
      '-loop', '0', reviewGif]);
    size = fs.statSync(reviewGif).size;
    if (size <= sizeLimit) break;
  }
  if (size > sizeLimit) throw new Error(`GIF exceeded ${sizeLimit / 1_000_000} MB after encoding.`);

  const duration = probeDuration(reviewGif);
  if (duration < Math.floor(minimumDuration) || duration > maximumDuration) {
    throw new Error('Encoded duration ' + duration.toFixed(2) + ' s is out of range.');
  }
  if (isDebugger) {
    if (isEngineDebugger || isLaunchDebugger) {
      writeDebuggerReview(ffmpeg, reviewGif, { duration, reviewDir: REVIEW_DIR, samples: debuggerReview, name: featureName });
    } else {
      writeDebuggerContactSheets(ffmpeg, reviewGif, { duration, reviewDir: REVIEW_DIR, ...debuggerReview });
    }
    fs.mkdirSync(outputDir, { recursive: true });
  } else {
    const sheet = featureName === 'code-actions' ? 'fps=12/' + duration + ':round=up,scale=480:-1,tile=3x4'
      : 'fps=6/' + duration + ':round=up,scale=640:-1,tile=2x3';
    ffmpeg(['-i', reviewGif, '-vf', sheet,
      '-frames:v', '1', '-update', '1', path.join(REVIEW_DIR, featureName + '.png')]);
  }
  fs.copyFileSync(reviewGif, gifOut);
  fs.rmSync(reviewGif);
  fs.rmSync(videoPath);
  fs.rmSync(palette);

  console.log('[DONE] ' + featureName + '.gif: ' + (size / 1024).toFixed(1) + ' KiB, ' + duration.toFixed(2) + ' s');
  return { name: featureName, size, duration };
} finally {
  if (app) {
    try {
      await app.close();
    } catch {}
  }
  try {
    await debuggerHost?.stop();
  } finally {
    cleanDebuggerWorkspace?.();
  }
  try {
    fs.rmSync(userDataDir, { recursive: true, force: true, maxRetries: 3, retryDelay: 150 });
  } catch (err) {
    console.warn('Could not clean up the isolated VS Code profile:', err.code);
  }
}
}

(async () => {
  const results = [];
  const skipped = [];

  for (const feat of targetFeatures) {
    let success = false;
    for (let attempt = 1; attempt <= 3; attempt++) {
      try {
        const res = await runScenario(feat);
        if (res) {
          results.push(res);
          success = true;
          break;
        }
      } catch (err) {
        console.error(`Attempt ${attempt} for ${feat} failed: ${err.message}`);
        await new Promise(r => setTimeout(r, 1000));
      }
    }
    if (!success) {
      skipped.push({ name: feat, reason: 'Failed after 3 attempts' });
    }
  }

  console.log('\n================ RECORDING SUMMARY ================');
  console.table(results);
  if (skipped.length > 0) {
    console.log('Skipped features:');
    console.table(skipped);
    process.exitCode = 1;
  }
})();
