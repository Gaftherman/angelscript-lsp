import { defineConfig } from '@vscode/test-cli';

// A workspace folder rather than no folder at all. Two of the tests are only meaningful with one:
// a relative `searchDirectories` entry resolves against each open folder, so with no folder open
// the assertion compares two empty lists and passes without testing anything. The fixture also
// carries the legacy `angelscript.predefinedFile` in its own settings.json - that key is
// deliberately absent from package.json's contributed configuration, which means the settings API
// refuses to write it and reading it from a real settings file is the only way to exercise the
// migration path at all.
export default defineConfig({
	files: 'out/test/**/*.test.js',
	workspaceFolder: './test-fixtures/workspace',
});
