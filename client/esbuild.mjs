// Bundles the extension into one file for shipping.
//
// Measured before this existed: loading `out/extension.js` pulled 126 CommonJS modules off disk,
// 125 of them vscode-languageclient's. Warm that is ~79ms; cold - a fresh install, every file
// unread and each one passing the virus scanner on the way in - it was 600ms, and it was the bulk
// of the 1.6s VS Code reported for activating this extension while comparable ones sat near 100ms.
// One file cannot cost 126 file opens.
//
// tsc still runs: it type-checks, and it produces out/test/ for the integration suite, which loads
// the compiled sources directly rather than through the bundle. esbuild does no type checking at
// all, which is why replacing tsc with it would have quietly removed the type check from CI.

import { build, context } from 'esbuild';

const watch = process.argv.includes('--watch');

/** @type {import('esbuild').BuildOptions} */
const options = {
    entryPoints: ['src/extension.ts'],
    outfile: 'dist/extension.js',
    bundle: true,
    format: 'cjs',
    platform: 'node',

    // The host is VS Code's Electron, whose Node is well past this. Pinned low rather than to the
    // newest so the bundle does not depend on syntax an older VS Code build cannot parse.
    target: 'node18',

    // Provided by the host at runtime and not resolvable from node_modules.
    external: ['vscode'],

    minify: !watch,

    // Points at the original TypeScript, so a stack trace in the user's output channel names a line
    // someone can go and read.
    sourcemap: true,
    logLevel: 'info'
};

if (watch) {
    const ctx = await context(options);
    await ctx.watch();
} else {
    await build(options);
}
