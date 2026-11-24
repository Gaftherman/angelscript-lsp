import * as assert from 'assert';
import { randomUUID } from 'crypto';
import { resolveRunner } from '../debug/runnerResolver';

suite('resolveRunner', () => {
    test('uses the configured executable before searching PATH', () => {
        const setting = `/runner-${randomUUID()}/custom-runner`;
        const directory = `/bin-${randomUUID()}`;
        const candidates = new Set([setting, `${directory}/angelscript-cli`]);
        assert.deepStrictEqual(resolveRunner(setting, {
            env: { PATH: directory },
            platform: 'linux',
            fileExists: candidate => candidates.has(candidate)
        }), { path: setting });
    });

    test('searches POSIX PATH entries in order', () => {
        const missing = `/bin-${randomUUID()}`;
        const first = `/bin-${randomUUID()}`;
        const second = `/bin-${randomUUID()}`;
        const runner = `${first}/angelscript-cli`;
        const candidates = new Set([runner, `${second}/angelscript-cli`]);
        assert.deepStrictEqual(resolveRunner('', {
            env: { PATH: `${missing}:${first}:${second}` },
            platform: 'linux',
            fileExists: candidate => candidates.has(candidate)
        }), { path: runner });
    });

    test('searches Windows PATH using the order of PATHEXT', () => {
        const missing = `C:\\bin-${randomUUID()}`;
        const directory = `D:\\tools ${randomUUID()}`;
        const runner = `${directory}\\angelscript-cli.com`;
        const candidates = new Set([runner, `${directory}\\angelscript-cli.exe`]);
        assert.deepStrictEqual(resolveRunner('', {
            env: { Path: `${missing};"${directory}"`, Pathext: '.COM;.EXE' },
            platform: 'win32',
            fileExists: candidate => candidates.has(candidate)
        }), { path: runner });
    });

    test('looks for .exe on Windows when PATHEXT is absent', () => {
        const directory = `C:\\bin-${randomUUID()}`;
        const runner = `${directory}\\angelscript-cli.exe`;
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
            fileExists: candidate => candidate === `${directory}/angelscript-cli`
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
