import * as assert from 'assert';
import * as path from 'path';
import { ConfigurationTarget, commands, extensions, workspace } from 'vscode';

import * as os from 'os';

import { buildServerArgs, portableStubPath } from '../extension';

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

    test('defined words reach the server as one --define each', async () => {
        // The server has had --define since the preprocessor landed, and nothing here emitted it,
        // so every VS Code workspace ran with an empty set of defined words. An empty set means
        // every `#if` block in every file is treated as excluded and its diagnostics suppressed -
        // silently, with no way for a user to notice.
        const args = await withSetting('define', ['SERVER', 'DEBUG'], buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--define='), ['SERVER', 'DEBUG']);
    });

    test('a blank defined word is dropped, and the rest are trimmed', async () => {
        // `#if ` followed by nothing is not a directive the preprocessor recognises, so an empty
        // entry could only ever define a word no script can name.
        const args = await withSetting('define', ['  ', ' PADDED '], buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--define='), ['PADDED']);
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

    test('the active stub reaches the server as --predefined-active', async () => {
        // Needed on the command line even though the running server is told through
        // didChangeConfiguration: a server starting fresh has never seen that notification, and
        // without the flag it would merge every stub in the workspace on the first scan.
        // Backslashes doubled: in a single-quoted TypeScript string `\h` is just `h`, so the
        // unescaped spelling this used to carry collapsed to `C:hostsengine.as.predefined` - a
        // drive-relative path that resolveAgainstWorkspace then rewrote, which is why this test
        // only ever failed on Windows and CI, running on Linux, never took the branch.
        const absolute = process.platform === 'win32'
            ? 'C:\\hosts\\engine.as.predefined'
            : '/hosts/engine.as.predefined';
        const args = await withSetting('predefined.active', absolute, buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--predefined-active='), [absolute]);
    });

    test('no active stub means no flag at all', async () => {
        // Empty is the default and it has to stay indistinguishable from never having set it, or
        // every workspace that merges stubs on purpose would change behaviour.
        const args = await withSetting('predefined.active', '', buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--predefined-active='), []);
    });

    test('a blank active stub is dropped rather than passed as an empty path', async () => {
        const args = await withSetting('predefined.active', '   ', buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--predefined-active='), []);
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

        // Declared in package.json's contributes.commands, so a command palette entry exists
        // whether or not anything registered it. One that is declared and not registered fails
        // when the user picks it, which is the worst place to find out.
        assert.ok(registered.includes('angelscript.selectPredefined'),
                  'angelscript.selectPredefined was declared in package.json but never registered');
    });
});

// =====================================================================================
// `${workspaceFolder}` and friends in a path-valued setting.
//
// VS Code expands these in launch.json and tasks.json and nowhere else: a setting reaches the
// extension exactly as it was typed. So the spelling every user reaches for first arrived at the
// server as a literal path with a dollar sign in it, matched no file, and the stub silently did not
// load - which is what made a workspace mixing one relative stub with one absolute path fail.
// =====================================================================================

suite('path variables in settings', () => {
    test('${workspaceFolder} becomes each workspace folder', async () => {
        const folders = workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'this test needs the fixture workspace from .vscode-test.mjs');

        const args = await withSetting('searchDirectories', ['${workspaceFolder}/include'], buildServerArgs);
        const expected = folders.map(folder => path.resolve(folder.uri.fsPath, 'include'));
        assert.deepStrictEqual(valuesOf(args, '--search-dir='), expected);
    });

    test('${userHome} becomes the home directory, and the result is absolute', async () => {
        const args = await withSetting('searchDirectories', ['${userHome}/angelscript'], buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--search-dir='),
                               [path.resolve(os.homedir(), 'angelscript')]);
    });

    test('${env:NAME} becomes the environment variable', async () => {
        // The way a host SDK path outside the workspace is usually already written down.
        const name = 'ANGELSCRIPT_TEST_SDK_ROOT';
        const value = process.platform === 'win32' ? 'C:\\sdk\\angelscript' : '/opt/sdk/angelscript';

        const previous = process.env[name];
        process.env[name] = value;
        try {
            const args = await withSetting('searchDirectories', [`\${env:${name}}/include`], buildServerArgs);
            assert.deepStrictEqual(valuesOf(args, '--search-dir='), [path.resolve(value, 'include')]);
        } finally {
            if (previous === undefined) {
                delete process.env[name];
            } else {
                process.env[name] = previous;
            }
        }
    });

    test('a variable this window cannot answer is left as written, not dropped', async () => {
        // Dropped, the setting vanishes and the user is told nothing. Left in place it reaches the
        // log as the path it is, which is a thing they can read and correct.
        const folders = workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'this test needs the fixture workspace from .vscode-test.mjs');

        const args = await withSetting(
            'searchDirectories', ['${workspaceFolder:nothing-is-named-this}/include'], buildServerArgs);

        const emitted = valuesOf(args, '--search-dir=');
        assert.strictEqual(emitted.length, folders.length);
        assert.ok(emitted.every(entry => entry.includes('nothing-is-named-this')),
                  `expected the unresolved name to survive, got ${JSON.stringify(emitted)}`);
    });

    test('an absolute path is still passed through untouched', async () => {
        // The control. Rewriting a path that has no variable in it would be a worse bug than the
        // one this fixes, and would look identical from the settings UI.
        const absolute = process.platform === 'win32' ? 'C:\\scripts\\shared' : '/scripts/shared';
        const args = await withSetting('searchDirectories', [absolute], buildServerArgs);
        assert.deepStrictEqual(valuesOf(args, '--search-dir='), [absolute]);
    });
});

suite('portableStubPath', () => {
    test('a stub inside a workspace folder is stored as ${workspaceFolder}/...', () => {
        // The picker used to write the absolute path it had in hand, which pins the setting to one
        // machine: committed to a repository it names a drive letter and a user directory nobody
        // else has.
        const folders = workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'this test needs the fixture workspace from .vscode-test.mjs');

        const inside = path.join(folders[0].uri.fsPath, 'stubs', 'host.as.predefined');
        assert.strictEqual(portableStubPath(inside), '${workspaceFolder}/stubs/host.as.predefined');
    });

    test('a stub outside every folder keeps its absolute path', () => {
        // There is nothing else it could be, and the two spellings have to be able to sit in the
        // same setting - the combination that was reported as broken.
        const outside = process.platform === 'win32'
            ? 'C:\\sdk\\host.as.predefined'
            : '/opt/sdk/host.as.predefined';
        assert.strictEqual(portableStubPath(outside), outside);
    });

    test('"all" is a request rather than a path and passes through', () => {
        assert.strictEqual(portableStubPath('all'), 'all');
    });
});

// =====================================================================================
// What activation actually costs.
//
// A user reported 2942 ms to load a small project against 500 ms on a fast machine. The server's
// workspace scan was timed and turned out to be nearly all of it - one canonicalisation per walked
// file. This is the other half of that question: the client reported nothing at all, so anything
// left over had nowhere to be looked up.
//
// The numbers are printed rather than bounded. A threshold here would be a test about the machine
// the suite happens to run on, and would either be too loose to catch a regression or fail on a
// loaded CI runner. What is asserted is that every phase is measured - a phase that silently stops
// being recorded is how this stops answering the question it exists for.
//
// The last three phases are reached only once a server binary exists: startClient returns early
// at `!server.found` before them. The CI job running these tests does not build the server, so it
// always takes that path, and the test therefore asserts the group is all-present or all-absent
// rather than requiring it.
// =====================================================================================

suite('activation timings', () => {
    test('every activation phase is measured', async () => {
        const extension = extensions.getExtension('Gaftherman.angelscript-lsp');
        assert.ok(extension, 'the extension under test is not installed in this host');

        await extension.activate();

        // From the host's own module instance, not this test's - see activate()'s return.
        const timings = (extension.exports as { activationTimings: Record<string, number> }).activationTimings;

        // Printed so a slow start has somewhere to be looked up, here and in CI logs.
        const report = Object.entries(timings)
            .sort((a, b) => b[1] - a[1])
            .map(([phase, ms]) => `${phase}=${ms}ms`)
            .join('  ');
        console.log('    activation: ' + report);

        for (const phase of ['moduleToActivate', 'outputChannel', 'registerCommands',
                             'statusBarItem', 'resolveServerBinary']) {
            assert.ok(phase in timings, `no measurement recorded for '${phase}'`);
            assert.ok(Number.isFinite(timings[phase]) && timings[phase] >= 0,
                      `'${phase}' recorded ${timings[phase]}, which is not a duration`);
        }

        const afterBinary = ['buildServerArgs', 'createFileSystemWatcher', 'constructLanguageClient'];
        const presentBinary = afterBinary.filter(phase => phase in timings);
        if (presentBinary.length === 0) {
            console.log('    activation: ran without a server binary');
        } else {
            const missing = afterBinary.filter(phase => !(phase in timings));
            assert.ok(missing.length === 0,
                      `server binary was found but missing phases: ${missing.join(', ')}`);
            for (const phase of afterBinary) {
                assert.ok(phase in timings, `no measurement recorded for '${phase}'`);
                assert.ok(Number.isFinite(timings[phase]) && timings[phase] >= 0,
                          `'${phase}' recorded ${timings[phase]}, which is not a duration`);
            }
        }
    });

    test('nothing before the server spawn takes anything like a second', async () => {
        // The one bound worth having, and it is deliberately generous. Everything measured here is
        // in-process bookkeeping - registering commands, reading settings, allocating a client. If
        // any of it reaches half a second on a developer machine, something has started doing work
        // it should not, and that is worth failing over even though the exact figure is not.
        //
        // `clientStart` is excluded on purpose: it spawns a process and waits out a protocol
        // handshake, which is legitimately the slow part and depends on the machine.
        const extension = extensions.getExtension('Gaftherman.angelscript-lsp');
        assert.ok(extension);
        await extension.activate();

        const timings = (extension.exports as { activationTimings: Record<string, number> }).activationTimings;
        const inProcess = Object.entries(timings)
            .filter(([phase]) => phase !== 'clientStart')
            .reduce((total, [, ms]) => total + ms, 0);

        assert.ok(inProcess < 500,
                  `activation spent ${inProcess}ms before the server was spawned: ` +
                  JSON.stringify(timings));
    });
});
