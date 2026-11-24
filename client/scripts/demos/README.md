# AngelScript LSP Demo Recordings

Automated tool to record reproducible demo GIFs for the extension README.

## Requirements
- `ffmpeg` and `ffprobe` on PATH (with GIF palette support)
- VS Code test runner download (handled automatically via `@vscode/test-electron`)

## Usage
- Record a single demo: `npm run demos -- --only hover`
- Record all demos: `npm run demos -- --all`
- Capture the rules file: `npm run demos -- --only rules-file`
- Inspect the six-frame contact sheets in `.vscode-test/temp-videos/review/` before using the GIFs.

## Debugger Demo

Run `npm run demos -- --only debugger` from `client/`. The recorder uses the
sibling `angelscript-debug-example` checkout and its Release host. Build the
host with `run-demo.cmd --build-only --asdebugger-dir ..\angelscript-debugger`
from that checkout first. The attach configuration uses port 9002. The
recorder's isolated VS Code profile masks the instruction-breakpoint capability
advertised by the host so VS Code can finish configuring the session.

The GIF is written to `angelscript-debug-example/media/debugger.gif`. Review
`debugger.png` for the full sequence and `debugger-stop.png` for the paused
states in `.vscode-test/temp-videos/review/`. The debugger demo runs for
20–30 seconds and has a 5 MB limit.

Run `npm run demos -- --only debugger-engine` to capture the host call,
callback, and unverified stub breakpoint. This uses the same Release host and
writes `angelscript-debug-example/media/debugger-engine.gif`. Review the
12-frame `debugger-engine.png` and the full-resolution `debugger-engine-*.png`
frames in the same review directory. The engine demo runs for 15–25 seconds
at 960 px wide and has a 5 MB limit.

From `client/`, build the extension with `npm run bundle`, then run
`npm run demos -- --only debugger-launch` to start the host with
**Launch AngelScript Demo** and capture script output in the **Debug Console**.
First build the example’s Release host with
`run-demo.cmd --build-only --asdebugger-dir ..\angelscript-debugger` from the
example checkout. The recorder prepares a separate build in its workspace copy,
and VS Code runs the pre-launch build task and starts the host on port 9002.
Ensure the port is free before recording.

The recorder writes `angelscript-debug-example/media/debugger-launch.gif`.
Review the 12-frame `debugger-launch.png` and the full-resolution breakpoint,
console, and end frames in `.vscode-test/temp-videos/review/`. The launch demo
runs for 15–25 seconds at 960 px wide and has a 5 MB limit.

## Visual Rules
1. Keep the synthetic pointer visible. Show all keycaps in a shortcut, light them in order, and hold the completed shortcut before sending it to VS Code.
2. Keep hovers, signatures, and suggestions legible within the frame.
3. Keep every looping GIF within 3 MB and 6–10 seconds. Signature help can take up to 11 seconds. Allow about 35 seconds for code actions so the highlighted entries and applied edits have time to be read.

Use `user-settings.json` for capture appearance. The fixture workspace settings only carry AngelScript project configuration.
