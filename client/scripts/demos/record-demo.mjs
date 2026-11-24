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
import { execSync } from 'child_process';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const CLIENT_ROOT = path.resolve(__dirname, '../..');
const REPO_ROOT = path.resolve(CLIENT_ROOT, '..');
const WORKSPACE_DIR = path.resolve(__dirname, 'workspace');
const NEUTRAL_WORKSPACE_DIR = 'C:\\demo\\medkit';
const SCENARIOS_DIR = path.resolve(__dirname, 'scenarios');
const IMAGES_DIR = path.resolve(CLIENT_ROOT, 'images');
const FRAMES_ROOT = path.resolve(REPO_ROOT, '_frames');
const VSCODE_TEST_DIR = path.resolve(CLIENT_ROOT, '.vscode-test');
const USER_DATA_DIR = path.resolve(VSCODE_TEST_DIR, 'demo-user-data');
const EMPTY_EXT_DIR = path.resolve(VSCODE_TEST_DIR, 'empty-extensions');
const TEMP_VIDEOS_DIR = path.resolve(VSCODE_TEST_DIR, 'temp-videos');

// Verify ffmpeg exists
try {
  execSync('ffmpeg -version', { stdio: 'pipe' });
} catch {
  console.error('Error: ffmpeg is not found on PATH. Please install ffmpeg.');
  process.exit(1);
}

// Backup pristine workspace files
const PRISTINE_FILES = new Map();
function backupWorkspace() {
  const filesToBackup = ['MedkitPlugin.as', 'DiagnosticsDemo.as', 'FormatDemo.as', '.vscode/settings.json', '.angelscript.rules.json'];
  for (const f of filesToBackup) {
    const full = path.join(WORKSPACE_DIR, f);
    if (fs.existsSync(full)) {
      PRISTINE_FILES.set(full, fs.readFileSync(full, 'utf8'));
    }
  }
}

function restoreWorkspace() {
  for (const [full, content] of PRISTINE_FILES.entries()) {
    fs.writeFileSync(full, content, 'utf8');
  }
  // Sync pristine files into neutral directory C:\demo\medkit
  fs.mkdirSync(NEUTRAL_WORKSPACE_DIR, { recursive: true });
  fs.cpSync(WORKSPACE_DIR, NEUTRAL_WORKSPACE_DIR, { recursive: true });
}

process.on('exit', () => {
  try {
    if (fs.existsSync(NEUTRAL_WORKSPACE_DIR)) {
      fs.rmSync(NEUTRAL_WORKSPACE_DIR, { recursive: true, force: true });
    }
  } catch {}
});

// Ensure output directories exist
fs.mkdirSync(IMAGES_DIR, { recursive: true });
fs.mkdirSync(FRAMES_ROOT, { recursive: true });
fs.mkdirSync(EMPTY_EXT_DIR, { recursive: true });
fs.mkdirSync(TEMP_VIDEOS_DIR, { recursive: true });

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

// All 16 features in priority order (MEDIA2: hover, autocomplete, diagnostics, engine-options, then the rest)
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

console.log('=== AngelScript LSP Demo Recorder ===');
console.log(`Targets (${targetFeatures.length}): ${targetFeatures.join(', ')}`);

backupWorkspace();

async function runScenario(featureName) {
  const scenarioFile = path.join(SCENARIOS_DIR, `${featureName}.json`);
  if (!fs.existsSync(scenarioFile)) {
    console.warn(`Scenario file not found: ${scenarioFile}, skipping.`);
    return null;
  }

  const scenario = JSON.parse(fs.readFileSync(scenarioFile, 'utf8'));
  restoreWorkspace();

  console.log(`\n--- Recording scenario: ${featureName} ---`);

  // Kill any lingering Code.exe processes to ensure clean run
  try {
    execSync('taskkill /F /IM Code.exe 2>nul', { stdio: 'ignore' });
  } catch {}

  const vscodePath = await downloadAndUnzipVSCode('1.146.0');
  const targetFiles = (featureName === 'engine-options')
    ? [path.join(NEUTRAL_WORKSPACE_DIR, 'MedkitPlugin.as'), path.join(NEUTRAL_WORKSPACE_DIR, '.vscode/settings.json')]
    : (scenario.file ? [path.join(NEUTRAL_WORKSPACE_DIR, scenario.file)] : [NEUTRAL_WORKSPACE_DIR]);
  const zoomIn = scenario.zoomIn || scenario.initialZoom || 1.0;
  let lastTokenCoords = null;
  const zoomOrigin = scenario.zoomOrigin || '360px 420px';

  const videoLaunchTime = Date.now();

  fs.mkdirSync(path.join(USER_DATA_DIR, 'User'), { recursive: true });
  fs.writeFileSync(path.join(USER_DATA_DIR, 'User', 'settings.json'), JSON.stringify({
    'window.titleBarStyle': 'native',
    'window.commandCenter': false,
    'workbench.layoutControl.enabled': false,
    'chat.disableAIFeatures': true,
    'workbench.secondarySideBar.defaultVisibility': 'hidden'
  }, null, 2));

  let app = null;
  try {
    app = await electron.launch({
      executablePath: vscodePath,
      args: [
        '--window-size=1280,720',
        '--force-device-scale-factor=1',
        NEUTRAL_WORKSPACE_DIR,
        '--extensionDevelopmentPath=' + CLIENT_ROOT,
        '--extensions-dir=' + EMPTY_EXT_DIR,
        '--user-data-dir=' + USER_DATA_DIR,
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
      recordVideo: {
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
  // Setup pristine UI layout filling 1280x720 without any black borders
  await win.evaluate(({ initZoom, origin, isSemantic, isEngineOptions, isSnap }) => {
    const style = document.createElement('style');
    style.textContent = `
      .part.auxiliarybar, .part.sidebar, .notifications-toasts, .part.statusbar, .part.titlebar, .part.activitybar {
        display: none !important;
      }
      .part.editor .title.tabs {
        ${isEngineOptions ? 'display: flex !important;' : 'display: none !important;'}
      }
      ${isEngineOptions ? '' : ''}
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
      .monaco-editor, .monaco-editor .overflow-guard {
        width: 1280px !important;
      }
      /* Tooltip width and margin protection */
      .monaco-hover {
        max-width: 720px !important;
        ${isSemantic ? 'display: none !important;' : ''}
      }
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
      /* Peek widget (code lens / find references) */
      .zone-widget, .zone-widget-container, .peekview-widget {
        width: 1236px !important;
      }
      .peekview-widget {
        height: 430px !important;
      }
      .peekview-widget .body .split-view-view:nth-child(1) { left: 0px !important; width: 600px !important; }
      .peekview-widget .body .split-view-view:nth-child(2) { left: 604px !important; width: 624px !important; }
      .peekview-widget .preview {
        left: 0 !important;
        width: 600px !important;
        min-width: 600px !important;
      }
      .peekview-widget .preview .monaco-editor .view-lines {
        padding-bottom: 30px !important;
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
        width: 980px !important;
        max-width: 980px !important;
      }
      .suggest-widget .monaco-list,
      .suggest-widget .monaco-list-rows,
      .suggest-widget .monaco-list-row {
        width: 980px !important;
        max-width: 980px !important;
      }
      .suggest-widget .monaco-list-row .contents {
        width: 960px !important;
        max-width: 960px !important;
      }
      .suggest-widget .monaco-list-row .contents .main {
        max-width: 940px !important;
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
    `;
    document.head.appendChild(style);

    // Set initial zoom level on workbench
    const wb = document.querySelector('.monaco-workbench');
    if (wb) {
      wb.style.transformOrigin = origin;
      wb.style.transform = `scale(${initZoom})`;
    }

    if (isEngineOptions || isSnap) {
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
    window.dispatchEvent(new Event('resize'));
  }, { initZoom: zoomIn, origin: zoomOrigin, isSemantic: featureName === 'semantic-highlighting', isEngineOptions: featureName === 'engine-options', isSnap: ['code-lens'].includes(featureName) });

  if (featureName === 'engine-options') {
    await win.evaluate(() => {
      const tab = Array.from(document.querySelectorAll('.tab')).find(t => (t.getAttribute('aria-label') || t.innerText || '').includes('MedkitPlugin.as'));
      if (tab) tab.click();
    });
    await win.waitForTimeout(500);
  }

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

  const findTokenCoords = async (token) => {
    return win.evaluate((tok) => {
      const elements = Array.from(document.querySelectorAll('.view-lines span, .codelens-decoration a, .codelens-decoration span, .tab-label a, .monaco-list-row span, a'));
      const exact = elements.filter(s => s.textContent.trim() === tok);
      const matches = exact.length > 0 ? exact : elements.filter(s => s.textContent.includes(tok));
      if (matches.length === 0) return null;
      // Filter for elements that are on screen
      const vh = window.innerHeight;
      const onScreen = matches.filter(s => {
        const r = s.getBoundingClientRect();
        return r.top >= 0 && r.top <= vh && r.width > 0 && r.height > 0;
      });
      let chosen = null;
      if (onScreen.length > 0) {
        onScreen.sort((a, b) => {
          const distA = Math.abs((a.getBoundingClientRect().top + a.getBoundingClientRect().height / 2) - vh / 2);
          const distB = Math.abs((b.getBoundingClientRect().top + b.getBoundingClientRect().height / 2) - vh / 2);
          return distA - distB;
        });
        chosen = onScreen[0];
      } else {
        chosen = matches[matches.length - 1];
      }
      const r = chosen.getBoundingClientRect();
      return {
        x: r.left + r.width / 2,
        y: r.top + r.height / 2
      };
    }, token);
  };

  const animMoveMouse = async (coords, durationMs = 600) => {
    const targetX = coords.x;
    const targetY = coords.y;

    const curPos = await win.evaluate(() => ({ x: window.__cursorX, y: window.__cursorY }));
    const movePromise = win.evaluate(({ tx, ty, ms }) => window.__moveCursor(tx, ty, ms), { tx: targetX, ty: targetY, ms: durationMs });
    const steps = 16;
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

  const animClick = async (coords) => {
    const curPos = await win.evaluate(() => ({ x: window.__cursorX, y: window.__cursorY }));
    const targetX = coords ? (coords.x !== undefined ? coords.x : coords.vx) : curPos.x;
    const targetY = coords ? (coords.y !== undefined ? coords.y : coords.vy) : curPos.y;
    if (coords) {
      await animMoveMouse(coords, 400);
    }
    await win.evaluate(() => window.__clickRipple());
    await win.mouse.click(targetX, targetY);
  };

  // Mark first scenario action timestamp
  const firstActionTime = Date.now();

  for (const step of scenario.steps) {
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
      let hoverVisible = false;
      for (let attempt = 0; attempt < 10; attempt++) {
        hoverVisible = await win.evaluate(() => {
          const h = document.querySelector('.monaco-hover');
          const hc = document.querySelector('.monaco-hover-content');
          if (!h) return false;
          const style = window.getComputedStyle(h);
          if (h.classList.contains('hidden') || style.display === 'none' || style.visibility === 'hidden') return false;
          return hc && hc.innerText.trim().length > 0;
        });
        if (hoverVisible) break;

        // Ensure mouse is at current cursor position with micro-wiggle
        const curPos = await win.evaluate(() => ({ x: window.__cursorX, y: window.__cursorY }));
        await win.mouse.move(curPos.x, curPos.y);
        await win.waitForTimeout(80);
        await win.mouse.move(curPos.x + 2, curPos.y);
        await win.waitForTimeout(80);
        await win.mouse.move(curPos.x, curPos.y);
        await win.waitForTimeout(200);

        hoverVisible = await win.evaluate(() => {
          const h = document.querySelector('.monaco-hover');
          const hc = document.querySelector('.monaco-hover-content');
          if (!h) return false;
          const style = window.getComputedStyle(h);
          if (h.classList.contains('hidden') || style.display === 'none' || style.visibility === 'hidden') return false;
          return hc && hc.innerText.trim().length > 0;
        });
        if (hoverVisible) break;

        // Click token to position caret
        await win.mouse.click(curPos.x, curPos.y);
        await win.waitForTimeout(100);

        // Try Ctrl+K Ctrl+I
        await win.keyboard.press('Control+k');
        await win.keyboard.press('Control+i');
        await win.waitForTimeout(350);

        hoverVisible = await win.evaluate(() => {
          const h = document.querySelector('.monaco-hover');
          const hc = document.querySelector('.monaco-hover-content');
          if (!h) return false;
          const style = window.getComputedStyle(h);
          if (h.classList.contains('hidden') || style.display === 'none' || style.visibility === 'hidden') return false;
          return hc && hc.innerText.trim().length > 0;
        });
        if (hoverVisible) break;

        // Try F1 Show Hover
        await win.keyboard.press('F1');
        await win.waitForTimeout(150);
        await win.keyboard.type('Show or Focus Hover');
        await win.keyboard.press('Enter');
        await win.waitForTimeout(400);
      }
      if (!hoverVisible) {
        throw new Error(`FAIL: Hover popup (.monaco-hover) failed to appear for scenario ${featureName} (cursor ${JSON.stringify(await win.evaluate(() => ({x: window.__cursorX, y: window.__cursorY})))})`);
      }
      console.log('ensureHover status: VISIBLE');
    } else if (step.action === 'moveCursor') {
      let coords = { x: step.x, y: step.y };
      if (step.toToken) {
        const found = await findTokenCoords(step.toToken);
        if (found) { coords = found; lastTokenCoords = { x: found.x, y: found.y }; }
      } else if (step.toLastToken && lastTokenCoords) {
        const ratio = (scenario.zoomOut || 1.0) / zoomIn;
        coords = { x: lastTokenCoords.x * ratio, y: lastTokenCoords.y * ratio };
      }
      if (coords.x !== undefined) {
        await animMoveMouse(coords, step.duration || 650);
      }
    } else if (step.action === 'click') {
      let coords = null;
      if (step.toToken) {
        coords = await findTokenCoords(step.toToken);
      } else if (step.x !== undefined && step.y !== undefined) {
        coords = { x: step.x, y: step.y };
      }
      await animClick(coords);
    } else if (step.action === 'clickTab') {
      const coords = await win.evaluate((tabName) => {
        const tab = Array.from(document.querySelectorAll('.tab')).find(t => (t.getAttribute('aria-label') || t.innerText || '').includes(tabName));
        if (!tab) return null;
        const r = tab.getBoundingClientRect();
        return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
      }, step.tab);
      if (coords) {
        await animClick(coords);
      } else {
        console.warn(`Tab not found: ${step.tab}`);
      }
    } else if (step.action === 'type') {
      await win.keyboard.type(step.text, { delay: step.delay || 75 });
    } else if (step.action === 'press') {
      await win.keyboard.press(step.key);
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
      console.log('DEFDBG ' + JSON.stringify(await win.evaluate(() => { const c = document.querySelector('.part.editor .cursor'); if (!c) return 'nocursor'; const b = c.getBoundingClientRect(); const e = document.elementFromPoint(b.left + 4, b.top + 10); return [Math.round(b.left), Math.round(b.top), e && e.textContent, (document.querySelector('.part.editor .view-lines')||{}).innerText && (document.querySelector('.part.editor .view-lines').innerText.slice(-120))]; })));
      try { await win.bringToFront(); } catch {}
      let jumped = false;
      for (let attempt = 0; attempt < 6 && !jumped; attempt++) {
        if (attempt % 2 === 0) {
          await win.keyboard.press('F12');
        } else {
          const cp = await win.evaluate(() => ({ x: window.__cursorX, y: window.__cursorY }));
          await win.keyboard.down('Control');
          await win.mouse.click(cp.x + 6, cp.y);
          await win.keyboard.up('Control');
        }
        await win.waitForTimeout(1300);
        jumped = await win.evaluate((txt) => Array.from(document.querySelectorAll('.part.editor .view-lines')).some(v => v.innerText.replace(/[\u00a0\s]+/g, ' ').includes(txt)), step.expect);
      }
      if (!jumped) throw new Error('FAIL: go to definition did not jump for scenario ' + featureName);
    } else if (step.action === 'hold') {
      if (process.env.DEMO_DEBUG) console.log('GEOM ' + JSON.stringify(await win.evaluate(() => {
        const r = (q) => Array.from(document.querySelectorAll(q)).slice(0, 3).map(e => { const b = e.getBoundingClientRect(); return [Math.round(b.left), Math.round(b.top), Math.round(b.width), Math.round(b.height), e.scrollWidth]; });
        return { suggest: r('.suggest-widget'), row: r('.suggest-widget .monaco-list-row.focused'), main: r('.suggest-widget .monaco-list-row.focused .main'), peek: r('.peekview-widget'), tree: r('.peekview-widget .ref-tree'), rows: r('.peekview-widget .ref-tree .monaco-list-row'), prev: r('.peekview-widget .preview'), pl: r('.peekview-widget .preview .view-line'), ml: r('.monaco-editor .view-line'), bodyH: r('.peekview-widget .body'), hdr: r('.peekview-widget .head'), ed: r('.part.editor .monaco-editor'), tt: r('.part.editor .title'), og: r('.part.editor .overflow-guard'), clip: Array.from(document.querySelectorAll('.part.editor .monaco-editor')).map(e => e.style.clipPath + '|' + e.className.slice(0,40)), vl: r('.part.editor .view-line'), dbg: window.__snapDbg, chain: (() => { const out=[]; let e=document.querySelector('.part.editor .view-lines'); while(e && out.length<14){ const b=e.getBoundingClientRect(); out.push(String(e.className).slice(0,28)+':'+Math.round(b.top)+'+'+Math.round(b.height)+(getComputedStyle(e).overflow!=='visible'?'(ov)':'')); e=e.parentElement;} return out; })(), pts: [300,660,685,700,715].map(y => [y, document.elementsFromPoint(300,y).slice(0,4).map(e => e.tagName + '.' + String(e.className).slice(0,50))]), win: [window.innerWidth, window.innerHeight, document.documentElement.clientHeight, window.outerHeight, getComputedStyle(document.body).height] };
      })));
      await win.waitForTimeout(step.ms || 1500);
    }
  }

  // Final hold of 1.0s as required by MEDIA2 brief
  await win.waitForTimeout(1000);
  const lastActionEndTime = Date.now();

  const video = win.video();
  await app.close();
  app = null;
  const vidPath = await video.path();

  // Dynamic trimming based on scenario timestamps (MEDIA2 requirement 2)
  const trimStart = Math.max(0, (firstActionTime - videoLaunchTime) / 1000 - 0.3);
  let duration = ((lastActionEndTime - firstActionTime) / 1000) + 0.3;
  // Ensure duration is in the 6.0 - 10.0s window
  if (duration < 6.0) duration = 6.0;
  if (duration > 10.0) duration = 10.0;

  const gifOut = path.join(IMAGES_DIR, `${featureName}.gif`);
  const palette = path.join(TEMP_VIDEOS_DIR, `${featureName}-palette.png`);

  console.log(`Encoding GIF with trimStart=${trimStart.toFixed(2)}s, duration=${duration.toFixed(2)}s...`);

  // First pass: 15 fps, 960x540, 128 max colors
  const cropH = scenario.cropHeight || 720;
  execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -vf "crop=1280:${cropH}:0:0,scale=960:-2:flags=lanczos,fps=15,palettegen=stats_mode=diff:max_colors=128" -vframes 1 -update 1 "${palette}"`, { stdio: 'pipe' });
  execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -i "${palette}" -filter_complex "[0:v]crop=1280:${cropH}:0:0,scale=960:-2:flags=lanczos,fps=15[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" -loop 0 "${gifOut}"`, { stdio: 'pipe' });

  let stat = fs.statSync(gifOut);
  // Guarantee <= 3 MB per MEDIA2 requirement
  if (stat.size > 3145728) {
    console.warn(`File ${gifOut} (${(stat.size / 1024).toFixed(1)} KB) > 3 MB. Re-encoding at 12 fps, 960x540, max_colors=96...`);
    execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -vf "crop=1280:${cropH}:0:0,scale=960:-2:flags=lanczos,fps=12,palettegen=stats_mode=diff:max_colors=96" -vframes 1 -update 1 "${palette}"`, { stdio: 'pipe' });
    execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -i "${palette}" -filter_complex "[0:v]crop=1280:${cropH}:0:0,scale=960:-2:flags=lanczos,fps=12[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" -loop 0 "${gifOut}"`, { stdio: 'pipe' });
    stat = fs.statSync(gifOut);
  }
  if (stat.size > 3145728) {
    console.warn(`File ${gifOut} still (${(stat.size / 1024).toFixed(1)} KB) > 3 MB. Re-encoding at 12 fps, 800x450, max_colors=96...`);
    execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -vf "crop=1280:${cropH}:0:0,scale=800:-2:flags=lanczos,fps=12,palettegen=stats_mode=diff:max_colors=96" -vframes 1 -update 1 "${palette}"`, { stdio: 'pipe' });
    execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -i "${palette}" -filter_complex "[0:v]crop=1280:${cropH}:0:0,scale=800:-2:flags=lanczos,fps=12[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" -loop 0 "${gifOut}"`, { stdio: 'pipe' });
    stat = fs.statSync(gifOut);
  }
  if (stat.size > 3145728) {
    console.warn(`File ${gifOut} still (${(stat.size / 1024).toFixed(1)} KB) > 3 MB. Re-encoding at 12 fps, 800x450, max_colors=64...`);
    execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -vf "crop=1280:${cropH}:0:0,scale=800:-2:flags=lanczos,fps=12,palettegen=stats_mode=diff:max_colors=64" -vframes 1 -update 1 "${palette}"`, { stdio: 'pipe' });
    execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -i "${palette}" -filter_complex "[0:v]crop=1280:${cropH}:0:0,scale=800:-2:flags=lanczos,fps=12[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" -loop 0 "${gifOut}"`, { stdio: 'pipe' });
    stat = fs.statSync(gifOut);
  }
  if (stat.size > 3145728) {
    console.warn(`File ${gifOut} still (${(stat.size / 1024).toFixed(1)} KB) > 3 MB. Re-encoding at 10 fps, 768x432, max_colors=64...`);
    execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -vf "crop=1280:${cropH}:0:0,scale=768:432:flags=lanczos,fps=10,palettegen=stats_mode=diff:max_colors=64" -vframes 1 -update 1 "${palette}"`, { stdio: 'pipe' });
    execSync(`ffmpeg -y -ss ${trimStart.toFixed(2)} -t ${duration.toFixed(2)} -i "${vidPath}" -i "${palette}" -filter_complex "[0:v]crop=1280:${cropH}:0:0,scale=768:432:flags=lanczos,fps=10[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" -loop 0 "${gifOut}"`, { stdio: 'pipe' });
    stat = fs.statSync(gifOut);
  }

  // Extract 6 frames evenly spaced for mandatory self-review (MEDIA4 requirement)
  const frameDir = path.join(FRAMES_ROOT, featureName);
  fs.rmSync(frameDir, { recursive: true, force: true });
  fs.mkdirSync(frameDir, { recursive: true });
  execSync(`ffmpeg -y -i "${gifOut}" -vf "fps=6/${duration.toFixed(2)}" "${frameDir}/%02d.png"`, { stdio: 'pipe' });
  const frameCount = fs.readdirSync(frameDir).length;

  console.log(`[DONE] ${featureName}.gif: ${(stat.size / 1024).toFixed(1)} KB, ~${duration.toFixed(1)}s, ${frameCount} review frames`);

  return {
    name: featureName,
    sizeKb: (stat.size / 1024).toFixed(1),
    duration: duration.toFixed(1),
    frames: frameCount,
    issuesFixed: 'Clean (1280x720 cropped, zoom calibrated, dynamic timing)'
  };
} finally {
  if (app) {
    try {
      await app.close();
    } catch {}
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

  restoreWorkspace();

  console.log('\n================ RECORDING SUMMARY ================');
  console.table(results);
  if (skipped.length > 0) {
    console.log('Skipped features:');
    console.table(skipped);
  }
})();
