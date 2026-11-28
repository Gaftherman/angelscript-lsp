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

## Debugger demos

The debugger demos live in [angelscript-dap](https://github.com/angelscript-tools/angelscript-dap), which has its own recorder.

## Visual Rules
1. Keep the synthetic pointer visible. Show all keycaps in a shortcut, light them in order, and hold the completed shortcut before sending it to VS Code.
2. Keep hovers, signatures, and suggestions legible within the frame.
3. Keep every looping GIF within 3 MB and 6–10 seconds. Signature help can take up to 11 seconds. Allow about 35 seconds for code actions so the highlighted entries and applied edits have time to be read.

Use `user-settings.json` for capture appearance. The fixture workspace settings only carry AngelScript project configuration.
