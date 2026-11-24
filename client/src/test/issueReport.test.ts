import * as assert from 'assert';
import { randomUUID } from 'crypto';
import {
    MAX_URL_LENGTH, anonymizeSnippet, anonymizeSource, buildIssueReport, buildIssueUrl, redact, tailLines
} from '../issueReport';

const word = (prefix: string): string => `${prefix}${randomUUID().replace(/-/g, '').slice(0, 8)}`;

suite('issueReport', () => {
    test('redact hides a workspace path written with either slash and as a file URI', () => {
        const folder = `C:\\Users\\${word('user')}\\${word('project')}`;
        const forward = folder.replace(/\\/g, '/');
        const text = `open ${folder}\\a.as and ${forward}/b.as and file:///${forward.replace(':', '%3A')}/c.as`;
        const out = redact(text, [{ value: folder, placeholder: '<workspace>' }]);
        assert.ok(!out.includes(folder), out);
        assert.ok(!out.toLowerCase().includes(forward.split('/').pop()!.toLowerCase()), out);
        assert.strictEqual(out.split('<workspace>').length - 1, 3, out);
    });

    test('redact replaces the longer path first and hides emails and user names', () => {
        const home = `/home/${word('person')}`;
        const workspace = `${home}/${word('game')}`;
        const out = redact(`${workspace}/x.as ${home}/.cache mail ${word('a')}@example.org`, [
            { value: home, placeholder: '~' },
            { value: workspace, placeholder: '<workspace>' }
        ]);
        assert.ok(out.startsWith('<workspace>/x.as ~/.cache'), out);
        assert.ok(out.endsWith('<email>'), out);
    });

    test('redact ignores values too short to be a path', () => {
        assert.strictEqual(redact('a b c', [{ value: 'a', placeholder: 'X' }]), 'a b c');
    });

    test('tailLines keeps the last lines and drops trailing blanks', () => {
        assert.strictEqual(tailLines('1\n2\n3\n4\n\n\n', 2), '3\n4');
    });

    test('anonymizeSource renames consistently and keeps the language words', () => {
        const type = word('Type');
        const member = word('field');
        const fn = word('run');
        const out = anonymizeSource(`class ${type} { int ${member}; void ${fn}() { ${member} = 1; } }`);
        assert.ok(!out.includes(type) && !out.includes(member) && !out.includes(fn), out);
        assert.match(out, /^class T1 \{ int v1; void f1\(\) \{ v1 = 1; \} \}$/);
    });

    test('anonymizeSource empties strings and removes comments', () => {
        const secret = word('secret');
        const note = word('note');
        const out = anonymizeSource(`string s = "${secret}"; // ${note}\n/* ${note}\n${note} */ char c = '${secret}';`);
        assert.ok(!out.includes(secret) && !out.includes(note), out);
        assert.ok(out.includes('""'), out);
        assert.strictEqual(out.split('\n').length, 3, 'line structure is kept');
    });

    test('anonymizeSource empties heredoc strings', () => {
        const secret = word('secret');
        const out = anonymizeSource(`string s = """${secret}\n${secret}""";`);
        assert.ok(!out.includes(secret), out);
    });

    test('anonymizeSnippet cuts a window around the cursor', () => {
        const lines = Array.from({ length: 100 }, (_, i) => `int ${word('n')}_${i};`);
        const out = anonymizeSnippet(lines.join('\n'), 50);
        assert.strictEqual(out.split('\n').length, 41);
    });

    test('the report never contains the private values it was given', () => {
        const folder = `/work/${word('studio')}/${word('mod')}`;
        const user = word('someone');
        const report = buildIssueReport({
            extensionVersion: '1.2.3',
            vscodeVersion: 'VS Code 1.0',
            platform: 'linux x64',
            summary: `failed at ${folder}/main.as for ${user}`,
            logTail: `error in ${folder}/main.as owned by ${user}@host.example`,
            snippet: anonymizeSource('class Hidden {}'),
            redactions: [
                { value: folder, placeholder: '<workspace>' },
                { value: user, placeholder: '<user>' }
            ]
        });
        for (const secret of [folder, user]) {
            assert.ok(!report.body.includes(secret) && !report.title.includes(secret), report.body);
        }
        assert.match(report.body, /```angelscript\nclass T1 \{\}\n```/);
        assert.ok(report.title.startsWith('[Bug]: '));
    });

    test('buildIssueUrl stays within the limit and keeps the head and the end of a long log', () => {
        const marker = word('last');
        const report = buildIssueReport({
            extensionVersion: '1.2.3',
            vscodeVersion: 'VS Code 1.0',
            platform: 'linux x64',
            summary: 'server stopped',
            logTail: `${'line of log output\n'.repeat(2000)}${marker}`,
            redactions: []
        });
        const url = buildIssueUrl('https://example.test/issues/new', report);
        assert.ok(url.length <= MAX_URL_LENGTH, String(url.length));
        assert.ok(decodeURIComponent(url).includes('server stopped'));
        assert.ok(decodeURIComponent(url).includes(marker));
    });
});
