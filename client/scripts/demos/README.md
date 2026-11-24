# AngelScript LSP Demo Recordings

Automated tool to record reproducible demo GIFs for the extension README.

## Requirements
- `ffmpeg` on PATH (supports libvpx/webp/palettegen)
- VS Code test runner download (handled automatically via `@vscode/test-electron`)

## Usage
- Record a single demo: `npm run demos -- --only hover`
- Record all demos: `npm run demos -- --all`

## Visual Rules
1. **Pre-zoomed focus**: Recordings start zoomed in without disorienting zoom bounces.
2. **Smooth synthetic mouse**: Cubic ease-in-out cursor overlay with click ripples.
3. **Legible tooltips**: Complete, uncropped hovers, signatures, and suggestions at 960 px.
4. **Performance limits**: Every animated GIF is optimized <= 3 MB, looping infinitely.
