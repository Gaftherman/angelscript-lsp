import { defineConfig } from '@vscode/test-cli';

// A workspace folder rather than no folder at all. Two of the tests are only meaningful with one:
// a relative `searchDirectories` entry resolves against each open folder, so with no folder open
// the assertion compares two empty lists and passes without testing anything. The fixture also
// carries the legacy `angelscript.predefinedFile` in its own settings.json - that key is
// deliberately absent from package.json's contributed configuration, which means the settings API
// refuses to write it and reading it from a real settings file is the only way to exercise the
// migration path at all.
const base = {
	workspaceFolder: './test-fixtures/workspace',
	version: process.env.VSCODE_VERSION || 'stable',
	download: {
		timeout: 60_000,
	},
	launchArgs: [
		'--disable-gpu',
		'--disable-gpu-sandbox',
		'--disable-dev-shm-usage',
		'--no-sandbox',
		'--disable-workspace-trust',
	],
};

// Two runs with their own label so `vscode-test --label unit` stays quick and the live-typing
// benchmark, which drives the real server for many mutation cycles, is opt-in. The first settings
// write after the extension host starts goes through a cold configuration service, which is
// slower than mocha's 2 s default, so every run gets a timeout sized for that.
export default defineConfig([
	{
		...base,
		label: 'unit',
		files: 'out/test/extension.test.js',
		mocha: { timeout: 20_000 },
	},
	{
		...base,
		label: 'e2e',
		files: 'out/test/e2e.test.js',
		mocha: { timeout: 60_000 },
	},
	{
		...base,
		label: 'perf',
		files: 'out/test/rapidFuzzer.test.js',
		mocha: { timeout: 120_000 },
	},
]);
