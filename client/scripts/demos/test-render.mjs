import { _electron as electron } from 'playwright-core';
import { downloadAndUnzipVSCode } from '@vscode/test-electron';
import path from 'path';
import { execSync } from 'child_process';
import fs from 'fs';

(async () => {
  const p = await downloadAndUnzipVSCode('1.146.0');
  const ws = path.resolve('./scripts/demos/workspace');
  const ext = path.resolve('.');
  const emptyExt = path.resolve('./.vscode-test/empty-extensions');
  const tempDir = path.resolve('./.vscode-test/temp-render');
  fs.mkdirSync(tempDir, { recursive: true });

  const app = await electron.launch({
    executablePath: p,
    args: [
      ws,
      '--extensionDevelopmentPath=' + ext,
      '--extensions-dir=' + emptyExt,
      '--user-data-dir=./.vscode-test/demo-user-data',
      '--locale=en',
      '--no-sandbox',
      '--disable-gpu-sandbox',
      '--disable-updates',
      '--disable-telemetry',
      '--disable-workspace-trust',
      '--skip-welcome',
      '--skip-release-notes',
      '--window-size=1280,720',
      ws + '/MedkitPlugin.as'
    ],
    recordVideo: {
      dir: tempDir,
      size: { width: 1280, height: 720 }
    }
  });

  const win = await app.firstWindow();
  await win.waitForLoadState('domcontentloaded');
  await win.waitForTimeout(2000);

  // Inject synthetic cursor using DOM nodes to satisfy Trusted Types
  await win.evaluate(() => {
    const cursor = document.createElement('div');
    cursor.id = 'demo-cursor';
    
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('width', '24');
    svg.setAttribute('height', '24');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.style.filter = 'drop-shadow(0 2px 4px rgba(0,0,0,0.6))';

    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', 'M5.5 3.2L18.5 13.8L12.5 14.5L15.8 21.2L12.7 22.6L9.5 16.0L5.5 19.5V3.2Z');
    path.setAttribute('fill', '#FFFFFF');
    path.setAttribute('stroke', '#1E1E1E');
    path.setAttribute('stroke-width', '1.5');
    path.setAttribute('stroke-linejoin', 'round');
    svg.appendChild(path);
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
      z-index: 999999999;
      transform: translate(800px, 200px);
      will-change: transform;
    `;
    document.body.appendChild(cursor);

    window.__cursorX = 800;
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
  });

  // Clean up UI in DOM
  await win.evaluate(() => {
    // Hide secondary sidebar
    const aux = document.querySelector('.part.auxiliarybar');
    if (aux) aux.style.display = 'none';
    // Hide notifications
    const notifs = document.querySelector('.notifications-toasts');
    if (notifs) notifs.style.display = 'none';
    // Close sidebar
    const sb = document.querySelector('.part.sidebar');
    if (sb) sb.style.display = 'none';
  });

  // Jump to 53:22 to place Dispense on screen
  await win.keyboard.press('Control+g');
  await win.waitForTimeout(200);
  await win.keyboard.type('53:22', { delay: 30 });
  await win.keyboard.press('Enter');
  await win.waitForTimeout(500);

  // Initial zoomed-in state
  await win.evaluate(() => {
    const wb = document.querySelector('.monaco-workbench');
    if (wb) {
      wb.style.transformOrigin = '55% 50%';
      wb.style.transform = 'scale(1.15)';
    }
  });

  const recordStartTime = Date.now();
  await win.waitForTimeout(400);

  // Move synthetic mouse to Dispense
  const tokenPos = await win.evaluate(() => {
    const spans = Array.from(document.querySelectorAll('.view-lines span'));
    const target = spans.find(s => s.textContent.trim() === 'Dispense');
    if (!target) return { x: 450, y: 512 };
    const r = target.getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
  });

  console.log('Moving cursor to:', tokenPos);
  // Animate visual cursor and send mouse events
  const movePromise = win.evaluate(({ x, y }) => window.__moveCursor(x, y, 650), tokenPos);
  for (let i = 1; i <= 13; i++) {
    const t = i / 13;
    const eased = t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
    const cx = 800 + (tokenPos.x - 800) * eased;
    const cy = 200 + (tokenPos.y - 200) * eased;
    await win.mouse.move(cx, cy);
    await win.waitForTimeout(50);
  }
  await movePromise;
  await win.mouse.move(tokenPos.x, tokenPos.y);

  // Wait for hover tooltip to appear
  await win.waitForTimeout(900);

  // Smooth zoom out over 600ms
  await win.evaluate(() => {
    const wb = document.querySelector('.monaco-workbench');
    if (wb) {
      wb.style.transition = 'transform 600ms cubic-bezier(0.25, 1, 0.5, 1)';
      wb.style.transform = 'scale(1.0)';
    }
  });

  // Hold 1.2s before loop
  await win.waitForTimeout(1200);

  const video = win.video();
  await app.close();
  const vidPath = await video.path();
  console.log('Video saved to:', vidPath);

  // Convert to GIF with ffmpeg
  const palette = path.resolve(tempDir, 'palette.png');
  const gifOut = path.resolve(tempDir, 'hover.gif');

  // Trim initial setup dead time
  const trimStart = 2.4;
  const duration = 3.6;

  execSync(`ffmpeg -y -ss ${trimStart} -t ${duration} -i "${vidPath}" -vf "crop=1280:685:0:35,fps=15,scale=960:-1:flags=lanczos,palettegen=stats_mode=diff:max_colors=128" -vframes 1 -update 1 "${palette}"`);
  execSync(`ffmpeg -y -ss ${trimStart} -t ${duration} -i "${vidPath}" -i "${palette}" -filter_complex "[0:v]crop=1280:685:0:35,fps=15,scale=960:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" -loop 0 "${gifOut}"`);

  const stat = fs.statSync(gifOut);
  console.log(`GIF rendered: ${(stat.size / 1024).toFixed(1)} KB`);

  // Extract frames for inspection
  const framesDir = path.resolve('./_frames/hover_test');
  fs.rmSync(framesDir, { recursive: true, force: true });
  fs.mkdirSync(framesDir, { recursive: true });
  execSync(`ffmpeg -y -i "${gifOut}" -vf fps=4 "${framesDir}/%03d.png"`);
  const frameFiles = fs.readdirSync(framesDir);
  console.log(`Extracted ${frameFiles.length} frames to ${framesDir}`);
})();
