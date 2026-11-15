import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { activateExtension, randomSuffix, waitForDiagnostics } from './helpers';

/** One typing slip, the line it sits on and the exact 0-based range the editor must underline. */
interface TypoCase {
    name: string;
    lines: string[];
    errorLine: number;
    startCharacter: number;
    endCharacter: number;
}

const typoCases = (suffix: string): TypoCase[] => [
    {
        name: 'misspelled global before a member access',
        lines: [
            `class Engine_${suffix} { float time; }`,
            `Engine_${suffix} g_engine_${suffix};`,
            `float f_${suffix} = g_engin_${suffix}.time;`,
        ],
        errorLine: 2,
        startCharacter: `float f_${suffix} = `.length,
        endCharacter: `float f_${suffix} = g_engin_${suffix}`.length,
    },
    {
        name: 'missing operator between a variable and a number',
        lines: [`float g_t_${suffix} = 1.0;`, `float f_${suffix} = g_t_${suffix} 1;`],
        errorLine: 1,
        startCharacter: 0,
        endCharacter: 0,
    },
    {
        name: 'misspelled keyword',
        lines: [`flaot v_${suffix} = 1.0;`],
        errorLine: 0,
        startCharacter: 0,
        endCharacter: 5,
    },
];

suite('AngelScript diagnostics ranges', function () {
    let fixtureDir: string;

    suiteSetup(async () => {
        const folders = vscode.workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'Requires an open workspace folder');
        await activateExtension();
        fixtureDir = path.join(folders[0].uri.fsPath, `diag-${randomSuffix()}`);
        await fs.promises.mkdir(fixtureDir, { recursive: true });
    });

    suiteTeardown(async () => {
        await vscode.commands.executeCommand('workbench.action.closeAllEditors');
        if (fixtureDir) {
            // The server may still hold a directory watch on Windows for a moment after the editors close.
            await fs.promises.rm(fixtureDir, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
        }
    });

    for (const slip of typoCases(randomSuffix())) {
        test(`reports the error on the right line: ${slip.name}`, async () => {
            const uri = vscode.Uri.file(path.join(fixtureDir, `${randomSuffix()}.as`));
            await fs.promises.writeFile(uri.fsPath, slip.lines.join('\n') + '\n', 'utf8');
            const published = waitForDiagnostics(uri, diagnostics => diagnostics.length > 0);
            await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(uri));
            await published;

            const errors = vscode.languages
                .getDiagnostics(uri)
                .filter(diagnostic => diagnostic.severity === vscode.DiagnosticSeverity.Error);
            assert.ok(errors.length > 0, 'a slip must produce an error');
            const first = errors[0];
            assert.strictEqual(first.range.start.line, slip.errorLine, first.message);
            assert.ok(first.range.end.line >= first.range.start.line);
            if (slip.endCharacter > 0) {
                assert.strictEqual(first.range.start.character, slip.startCharacter, first.message);
                assert.strictEqual(first.range.end.character, slip.endCharacter, first.message);
            }
        });
    }
});
