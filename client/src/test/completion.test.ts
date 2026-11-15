import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { activateExtension, randomSuffix, waitForDiagnostics } from './helpers';

/** Labels the language server contributed; VS Code's own snippet provider answers in every context and is left out. */
async function getServerLabels(uri: vscode.Uri, position: vscode.Position): Promise<string[]> {
    const result = await vscode.commands.executeCommand<vscode.CompletionList>(
        'vscode.executeCompletionItemProvider',
        uri,
        position
    );
    return (result?.items ?? [])
        .filter(item => item.kind !== vscode.CompletionItemKind.Snippet)
        .map(item => (typeof item.label === 'string' ? item.label : item.label.label));
}

suite('AngelScript context-aware completion', function () {
    let fixtureDir: string;

    suiteSetup(async () => {
        const folders = vscode.workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'Requires an open workspace folder');
        await activateExtension();
        fixtureDir = path.join(folders[0].uri.fsPath, `completion-${randomSuffix()}`);
        await fs.promises.mkdir(fixtureDir, { recursive: true });
    });

    suiteTeardown(async () => {
        await vscode.commands.executeCommand('workbench.action.closeAllEditors');
        if (fixtureDir) {
            await fs.promises.rm(fixtureDir, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
        }
    });

    /** Opens a script and waits until the server has analysed it, so completion answers from the index. */
    async function open(lines: string[]): Promise<vscode.Uri> {
        const uri = vscode.Uri.file(path.join(fixtureDir, `${randomSuffix()}.as`));
        await fs.promises.writeFile(uri.fsPath, lines.join('\n') + '\n', 'utf8');
        const analysed = waitForDiagnostics(uri, () => true);
        await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri));
        await analysed;
        return uri;
    }

    test('a member access lists the members of the receiver', async () => {
        const type = `Unit_${randomSuffix()}`;
        const field = `hp_${randomSuffix()}`;
        const method = `Hit_${randomSuffix()}`;
        const head = '    unit.';
        const uri = await open([
            `class ${type} { int ${field}; void ${method}() {} }`,
            'void main()',
            '{',
            `    ${type} unit;`,
            head,
            '}',
        ]);
        const labels = await getServerLabels(uri, new vscode.Position(4, head.length));
        assert.ok(labels.includes(field), labels.join(','));
        assert.ok(labels.includes(method), labels.join(','));
        assert.ok(!labels.includes('class'), 'keywords do not belong after a dot');
    });

    test('a constructor header offers only its qualifiers', async () => {
        const type = `Spawn_${randomSuffix()}`;
        const head = `    ${type}() `;
        const uri = await open([`class ${type}`, '{', head, '    {', '    }', '}']);
        const labels = await getServerLabels(uri, new vscode.Position(2, head.length));
        assert.deepStrictEqual(labels, ['explicit']);
    });

    test('a derived method header offers override only when the base declares it', async () => {
        const base = `Base_${randomSuffix()}`;
        const known = `tick_${randomSuffix()}`;
        const fresh = `extra_${randomSuffix()}`;
        const knownHead = `    void ${known}() `;
        const freshHead = `    void ${fresh}() `;
        const uri = await open([
            `class ${base} { void ${known}() {} }`,
            `class Derived_${randomSuffix()} : ${base}`,
            '{',
            knownHead,
            '    {',
            '    }',
            freshHead,
            '    {',
            '    }',
            '}',
        ]);
        const inherited = await getServerLabels(uri, new vscode.Position(3, knownHead.length));
        assert.deepStrictEqual([...inherited].sort(), ['const', 'final', 'override']);
        const added = await getServerLabels(uri, new vscode.Position(6, freshHead.length));
        assert.deepStrictEqual([...added].sort(), ['const', 'final']);
    });

    test('a bare label colon offers nothing instead of document words', async () => {
        const name = `slot_${randomSuffix()}`;
        const head = `    ${name}:`;
        const uri = await open(['void main()', '{', `    float ${name} = 1.0f;`, head, '}']);
        const labels = await getServerLabels(uri, new vscode.Position(3, head.length));
        assert.deepStrictEqual(labels, []);
    });
});
