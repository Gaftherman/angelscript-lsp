import { execSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const clientRoot = path.resolve(__dirname, '..');

// 1. Determine active git tag
let tagName = '';
try {
    tagName = execSync('git describe --tags --match "v*" --exact-match', { cwd: clientRoot, encoding: 'utf-8' }).trim();
} catch {
    try {
        tagName = execSync('git describe --tags --exact-match', { cwd: clientRoot, encoding: 'utf-8' }).trim();
    } catch {
        try {
            tagName = execSync('git describe --tags --always', { cwd: clientRoot, encoding: 'utf-8' }).trim();
        } catch {
            tagName = '';
        }
    }
}

const pkg = JSON.parse(readFileSync(path.join(clientRoot, 'package.json'), 'utf-8'));
const expectedTag = `v${pkg.version}`;
if (!tagName || tagName !== expectedTag) {
    tagName = expectedTag;
}

const outFile = `angelscript-lsp-${tagName}.vsix`;
console.log(`Packaging VSIX with tag: ${tagName} -> ${outFile}...`);

// Run vsce package
execSync(`npx @vscode/vsce package --no-git-tag-version --out "${outFile}"`, {
    cwd: clientRoot,
    stdio: 'inherit'
});

console.log(`Successfully packaged: ${outFile}`);
