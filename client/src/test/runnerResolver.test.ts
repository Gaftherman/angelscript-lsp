import * as assert from 'assert';
import { randomUUID } from 'crypto';
import { resolveRunner } from '../debug/runnerResolver';

suite('resolveRunner', () => {
    test('uses the configured executable before searching PATH', () => {
        const setting = `/runner-${randomUUID()}/custom-runner`;
        const directory = `/bin-${randomUUID()}`;
        const candidates = new Set([setting, `${directory}/angelscript`]);
        assert.deepStrictEqual(resolveRunner(setting, {
            env: { PATH: directory },
            platform: 'linux',
            fileExists: candidate => candidates.has(candidate)
        }), { path: setting });
    });

    test('prefers asc in an earlier POSIX PATH directory over angelscript in a later one', () => {
        const missing = `/bin-${randomUUID()}`;
        const first = `/bin-${randomUUID()}`;
        const second = `/bin-${randomUUID()}`;
        const runner = `${first}/asc`;
        const candidates = new Set([runner, `${second}/angelscript`]);
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: `${missing}:${first}:${second}` },
            platform: 'linux',
            fileExists: candidate => candidates.has(candidate)
        }), { path: runner });
    });

    test('prefers angelscript over both aliases in the same directory', () => {
        const directory = `/bin-${randomUUID()}`;
        const runner = `${directory}/angelscript`;
        const candidates = new Set([runner, `${directory}/asc`, `${directory}/angelscript-cli`]);
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: directory },
            platform: 'linux',
            fileExists: candidate => candidates.has(candidate)
        }), { path: runner });
    });

    test('falls back to asc when angelscript is missing', () => {
        const directory = `/bin-${randomUUID()}`;
        const runner = `${directory}/asc`;
        const candidates = new Set([runner, `${directory}/angelscript-cli`]);
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: directory },
            platform: 'linux',
            fileExists: candidate => candidates.has(candidate)
        }), { path: runner });
    });

    test('falls back to angelscript-cli when the other names are missing', () => {
        const directory = `/bin-${randomUUID()}`;
        const runner = `${directory}/angelscript-cli`;
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: directory },
            platform: 'linux',
            fileExists: candidate => candidate === runner
        }), { path: runner });
    });

    for (const name of ['angelscript', 'asc', 'angelscript-cli']) {
        for (const [extensions, preferred] of [['.COM;.EXE', '.com'], ['.EXE;.COM', '.exe']] as const) {
            test(`prefers ${name}${preferred} on Windows when PATHEXT is ${extensions}`, () => {
                const missing = `C:\\bin-${randomUUID()}`;
                const directory = `D:\\tools ${randomUUID()}`;
                const runner = `${directory}\\${name}`;
                const candidates = new Set([`${runner}.com`, `${runner}.exe`]);
                assert.deepStrictEqual(resolveRunner('', {
                    env: { Path: `${missing};"${directory}"`, Pathext: extensions },
                    platform: 'win32',
                    fileExists: candidate => candidates.has(candidate)
                }), { path: `${runner}${preferred}` });
            });
        }
    }

    test('prefers the runner name before the PATHEXT order in the same Windows directory', () => {
        const directory = `C:\\bin-${randomUUID()}`;
        const runner = `${directory}\\angelscript.exe`;
        const candidates = new Set([runner, `${directory}\\asc.com`, `${directory}\\angelscript-cli.com`]);
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: directory, PATHEXT: '.COM;.EXE' },
            platform: 'win32',
            fileExists: candidate => candidates.has(candidate)
        }), { path: runner });
    });

    test('looks for .exe on Windows when PATHEXT is absent', () => {
        const directory = `C:\\bin-${randomUUID()}`;
        const runner = `${directory}\\angelscript.exe`;
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: directory },
            platform: 'win32',
            fileExists: candidate => candidate === runner
        }), { path: runner });
    });

    test('returns not-found when PATH contains no runner', () => {
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: `/bin-${randomUUID()}` },
            platform: 'linux',
            fileExists: () => false
        }), { error: 'not-found' });
    });

    test('returns not-found for a missing configured file even if PATH contains a runner', () => {
        const directory = `/bin-${randomUUID()}`;
        assert.deepStrictEqual(resolveRunner(`/missing-${randomUUID()}/runner`, {
            env: { PATH: directory },
            platform: 'linux',
            fileExists: candidate => candidate === `${directory}/angelscript`
        }), { error: 'not-found' });
    });

    test('rejects a relative configured path', () => {
        assert.deepStrictEqual(resolveRunner(`bin-${randomUUID()}/runner`, {
            env: {},
            platform: 'linux',
            fileExists: () => true
        }), { error: 'not-found' });
    });

    test('returns not-found for an empty PATH without consulting the file system', () => {
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: '' },
            platform: 'linux',
            fileExists: () => assert.fail('An empty PATH supplies no candidates')
        }), { error: 'not-found' });
    });
});
