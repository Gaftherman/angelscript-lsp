import * as assert from 'assert';
import * as path from 'path';
import { ConfigurationTarget, commands, extensions, workspace } from 'vscode';

import { buildServerArgs } from '../extension';

// =====================================================================================
// The client's settings-to-arguments mapping.
//
// Nearly every `angelscript.*` setting becomes a command-line flag on the server process, and a
// flag that is spelled wrong, dropped, or emitted when it should not be changes how the server
// behaves with nothing to show for it. `npm run check-settings` already proves each declared
// setting is MENTIONED in extension.ts - a regex over the source. These prove what it actually
// emits.
//
// The harness was declared before any of this existed: package.json carried `"test": "vscode-test"`,
// .vscode-test.mjs pointed at `out/test/**/*.test.js`, and no such file was ever written. `npm test`
// ran zero tests and reported success.
// =====================================================================================

/** @brief Sets one setting, runs the body, and puts the setting back whatever happens. */
async function withSetting<T>(key: string, value: unknown, body: () => T): Promise<T> {
    const config = workspace.getConfiguration('angelscript');
    const previous = config.inspect(key)?.globalValue;
    await config.update(key, value, ConfigurationTarget.Global);
    try {
        return body();
    } finally {
        await workspace.getConfiguration('angelscript').update(key, previous, ConfigurationTarget.Global);
    }
}

/** @brief Every argument carrying a given flag prefix, with the prefix stripped. */
function valuesOf(args: string[], prefix: string): string[] {
    return args.filter(arg => arg.startsWith(prefix)).map(arg => arg.slice(prefix.length));
}

suite('buildServerArgs', () => {
    test('an exclude glob reaches the server verbatim', async () => {
        const args = await withSetting('exclude', ['**/vendor/**', '**/.cache/**'], buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--exclude='), ['**/vendor/**', '**/.cache/**']);
    });

    test('a blank exclude entry is dropped rather than passed as an empty glob', async () => {
        // An empty --exclude= would reach the server's glob matcher as a pattern matching nothing,
        // or everything, depending on how it is read. Neither is what an empty text box means.
        const args = await withSetting('exclude', ['   ', '**/build/**'], buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--exclude='), ['**/build/**']);
    });

    test('an absolute search directory is passed through unchanged', async () => {
        const absolute = process.platform === 'win32' ? 'C:\\scripts\\shared' : '/scripts/shared';
        const args = await withSetting('searchDirectories', [absolute], buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--search-dir='), [absolute]);
    });

    test('a relative search directory is resolved against each workspace folder', async () => {
        // The server's working directory is not the project root, so a relative entry means nothing
        // until it is resolved - and in a multi-root workspace it legitimately means one path per
        // folder.
        const folders = workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'this test needs the fixture workspace from .vscode-test.mjs');

        const args = await withSetting('searchDirectories', ['include'], buildServerArgs);
        const expected = folders.map(folder => path.resolve(folder.uri.fsPath, 'include'));
        assert.deepStrictEqual(valuesOf(args, '--search-dir='), expected);
    });

    test('the legacy predefinedFile setting is read as the path it always claimed to be', () => {
        // It promised a path and delivered a suffix. Existing settings have to start working rather
        // than stay silently broken, which is the whole point of the migration.
        //
        // Read from the fixture workspace's own settings.json rather than written here: the key is
        // deliberately not in package.json's contributed configuration - a deprecated setting has no
        // business in the settings UI - and the configuration API refuses to write a key it does not
        // know. A real settings file is the only way this path can be exercised, and it is also
        // exactly how a user with the old setting reaches it.
        const folders = workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'this test needs the fixture workspace from .vscode-test.mjs');

        const expected = path.resolve(folders[0].uri.fsPath, 'stubs/legacy.as.predefined');
        assert.ok(valuesOf(buildServerArgs(), '--predefined-file=').includes(expected),
                  `expected ${expected} among the predefined files`);
    });

    test('a feature left on adds nothing to the command line', async () => {
        const args = await withSetting('features.hover', true, buildServerArgs);
        assert.ok(!args.some(arg => arg.includes('hover')), `unexpected hover flag in ${args.join(' ')}`);
    });

    test('a feature switched off adds its disabling flag', async () => {
        const args = await withSetting('features.hover', false, buildServerArgs);
        assert.ok(args.includes('--disable-hover'), `expected --disable-hover in ${args.join(' ')}`);
    });

    test('the accessor mode is passed for both values the manifest offers', async () => {
        for (const mode of [2, 3]) {
            const args = await withSetting('engine.propertyAccessorMode', mode, buildServerArgs);
            assert.ok(args.includes(`--engine-property=propertyAccessorMode=${mode}`),
                      `expected mode ${mode} in ${args.join(' ')}`);
        }
    });

    test('an accessor mode outside the manifest is dropped, not forwarded', async () => {
        // A hand-edited settings.json can hold anything. Dropping it here beats handing the server
        // a number for it to reject.
        const args = await withSetting('engine.propertyAccessorMode', 7, buildServerArgs);
        assert.ok(!args.some(arg => arg.startsWith('--engine-property=propertyAccessorMode=')),
                  `unexpected accessor mode in ${args.join(' ')}`);
    });

    test('an unset brace style stays off the command line', async () => {
        const args = await withSetting('format.braceStyle', '', buildServerArgs);
        assert.ok(!args.some(arg => arg.startsWith('--format-brace-style=')));
    });

    test('a diagnostic severity override is emitted as code=severity', async () => {
        const args = await withSetting('diagnosticSeverity', { 'as-err-undefined-identifier': 'warning' }, buildServerArgs);
        assert.ok(args.includes('--diagnostic-severity=as-err-undefined-identifier=warning'),
                  `expected the override in ${args.join(' ')}`);
    });

    test('the editor language is always forwarded', () => {
        // The server localises its diagnostics and has no other way to learn which language to use.
        const args = buildServerArgs();
        assert.ok(args.some(arg => arg.startsWith('--locale=')), `expected a locale in ${args.join(' ')}`);
    });
});

suite('activation', () => {
    test('the extension activates and registers its commands', async () => {
        const extension = extensions.getExtension('Gaftherman.angelscript-lsp')
            ?? extensions.all.find(candidate => candidate.packageJSON?.name === 'angelscript-lsp');
        assert.ok(extension, 'the extension under test was not found');

        await extension.activate();

        const registered = await commands.getCommands(true);
        assert.ok(registered.includes('angelscript.restartServer'),
                  'angelscript.restartServer was declared in package.json but never registered');
        assert.ok(registered.includes('angelscript.showServerLog'),
                  'angelscript.showServerLog was declared in package.json but never registered');
    });
});
