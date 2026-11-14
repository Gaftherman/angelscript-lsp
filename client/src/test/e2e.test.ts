import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

interface TestSymbols {
    nsA: string;
    nsB: string;
    nsC: string;
    classT: string;
    helperFn: string;
    innerFn: string;
    userFnA: string;
    userFnB: string;
    userFnMember: string;
}

/** @brief Generates a unique alphanumeric suffix for isolated test fixtures and symbols. */
function randomSuffix(): string {
    return `${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`;
}

/** @brief Builds a set of randomized AngelScript identifiers for one test run. */
function createTestSymbols(suffix: string): TestSymbols {
    return {
        nsA: `A_${suffix}`,
        nsB: `B_${suffix}`,
        nsC: `C_${suffix}`,
        classT: `T_${suffix}`,
        helperFn: `helper_${suffix}`,
        innerFn: `inner_${suffix}`,
        userFnA: `useNsA_${suffix}`,
        userFnB: `useNsB_${suffix}`,
        userFnMember: `useMember_${suffix}`,
    };
}

/**
 * @brief Resolves on the first diagnostics change for a URI whose current diagnostics satisfy a predicate.
 * @param uri Document whose diagnostics are watched.
 * @param accept Predicate over the diagnostics published for the document; defaults to accepting any state.
 */
function waitForDiagnostics(
    uri: vscode.Uri,
    accept: (diagnostics: readonly vscode.Diagnostic[]) => boolean = () => true
): Promise<void> {
    const target = uri.toString();
    return new Promise<void>(resolve => {
        const disposable = vscode.languages.onDidChangeDiagnostics(event => {
            if (event.uris.some(changed => changed.toString() === target) && accept(vscode.languages.getDiagnostics(uri))) {
                disposable.dispose();
                resolve();
            }
        });
    });
}

/** @brief Executes completion at a position and returns normalized item labels. */
async function getCompletionLabels(uri: vscode.Uri, position: vscode.Position): Promise<string[]> {
    const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
        'vscode.executeCompletionItemProvider',
        uri,
        position
    );
    if (!result) {
        return [];
    }
    const items = Array.isArray(result) ? result : result.items;
    return items.map(item => (typeof item.label === 'string' ? item.label : item.label.label));
}

/** @brief Executes hover at a position and joins all returned markdown or string contents. */
async function getHoverText(uri: vscode.Uri, position: vscode.Position): Promise<string> {
    const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
        'vscode.executeHoverProvider',
        uri,
        position
    );
    if (!hovers || hovers.length === 0) {
        return '';
    }
    const parts: string[] = [];
    for (const hover of hovers) {
        for (const content of hover.contents) {
            parts.push(typeof content === 'string' ? content : content.value);
        }
    }
    return parts.join('\n');
}

suite('AngelScript LSP E2E', function () {
    let fixtureDir: string;
    let mainUri: vscode.Uri;
    let symbols: TestSymbols;
    let mainLines: string[];
    let innerPos: vscode.Position;

    suiteSetup(async () => {
        const folders = vscode.workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'Requires an open workspace folder');

        const extension = vscode.extensions.getExtension('Gaftherman.angelscript-gaftherman')
            ?? vscode.extensions.getExtension('Gaftherman.angelscript')
            ?? vscode.extensions.all.find(
                candidate => candidate.packageJSON?.name === 'angelscript-gaftherman'
                    || candidate.packageJSON?.name === 'angelscript'
            );
        assert.ok(extension, 'AngelScript extension must be installed in the test host');
        await extension.activate();

        const suffix = randomSuffix();
        symbols = createTestSymbols(suffix);
        fixtureDir = path.join(folders[0].uri.fsPath, `e2e-${suffix}`);
        await fs.promises.mkdir(fixtureDir, { recursive: true });

        const incPath = path.join(fixtureDir, 'inc.as');
        const mainPath = path.join(fixtureDir, 'main.as');

        const incContent = [
            `namespace ${symbols.nsB} {`,
            `    class ${symbols.classT} {`,
            '        int pub;',
            '        private int priv;',
            '    }',
            `    void ${symbols.helperFn}() {}`,
            '}',
            '',
        ].join('\n');

        mainLines = [
            '#include "inc.as"',
            `namespace ${symbols.nsA} {`,
            `    namespace ${symbols.nsC} {`,
            `        void ${symbols.innerFn}() {}`,
            '    }',
            '}',
            `void ${symbols.userFnA}() {`,
            `    ${symbols.nsA}::${symbols.nsC}::`,
            '}',
            `void ${symbols.userFnB}() {`,
            `    ${symbols.nsB}::`,
            '}',
            `void ${symbols.userFnMember}() {`,
            `    ${symbols.nsB}::${symbols.classT} v;`,
            '    v.',
            '}',
            'Cl void f(){}',
            '',
        ];

        await fs.promises.writeFile(incPath, incContent, 'utf8');
        await fs.promises.writeFile(mainPath, mainLines.join('\n'), 'utf8');

        mainUri = vscode.Uri.file(mainPath);
        const innerLine = 3;
        innerPos = new vscode.Position(innerLine, mainLines[innerLine].indexOf(symbols.innerFn) + 1);

        const initialDiagnostics = waitForDiagnostics(mainUri);
        const doc = await vscode.workspace.openTextDocument(mainUri);
        await vscode.window.showTextDocument(doc);
        await initialDiagnostics;
    });

    suiteTeardown(async () => {
        await vscode.commands.executeCommand('workbench.action.revertAndCloseActiveEditor');
        await vscode.commands.executeCommand('workbench.action.closeAllEditors');
        if (fixtureDir) {
            await fs.promises.rm(fixtureDir, { recursive: true, force: true });
        }
    });

    test('completion resolves nested namespaces, included symbols, member visibility, and keyword prefix', async () => {
        const acPos = new vscode.Position(7, mainLines[7].length);
        const acLabels = await getCompletionLabels(mainUri, acPos);
        assert.ok(acLabels.includes(symbols.innerFn), `Expected ${symbols.innerFn} in A::C:: completions`);
        assert.ok(!acLabels.includes(symbols.userFnA), 'A::C:: must be scoped, not the global list');

        const bPos = new vscode.Position(10, mainLines[10].length);
        const bLabels = await getCompletionLabels(mainUri, bPos);
        assert.ok(bLabels.includes(symbols.helperFn), `Expected ${symbols.helperFn} in B:: completions`);
        assert.ok(!bLabels.includes(symbols.userFnB), 'B:: must be scoped, not the global list');
        assert.ok(bLabels.includes(symbols.classT), `Expected ${symbols.classT} in B:: completions`);

        const vPos = new vscode.Position(14, mainLines[14].length);
        const memberLabels = await getCompletionLabels(mainUri, vPos);
        assert.ok(memberLabels.includes('pub'), 'Expected public member pub in v. completions');
        assert.ok(!memberLabels.includes('priv'), 'Did not expect private member priv in v. completions');

        const clPos = new vscode.Position(16, 2);
        const clLabels = await getCompletionLabels(mainUri, clPos);
        assert.ok(clLabels.includes('class'), 'Expected the class keyword after the Cl prefix');
    });

    test('hover returns function name for nested namespace function', async () => {
        const hoverText = await getHoverText(mainUri, innerPos);
        assert.ok(
            hoverText.includes(symbols.innerFn),
            `Expected hover text to contain ${symbols.innerFn}, got: ${hoverText}`
        );
    });

    test('stress: 200 rapid WorkspaceEdits settle diagnostics and preserve hover', async () => {
        const tailLine = mainLines.length - 1;
        for (let i = 0; i < 200; i++) {
            const edit = new vscode.WorkspaceEdit();
            if (i % 2 === 0) {
                edit.insert(mainUri, new vscode.Position(tailLine, 0), `// stress_${i}\n`);
            } else {
                edit.delete(mainUri, new vscode.Range(tailLine, 0, tailLine + 1, 0));
            }
            const applied = await vscode.workspace.applyEdit(edit);
            assert.ok(applied, `WorkspaceEdit ${i} must apply`);
        }

        const sentinel = `sentinel_${symbols.innerFn}`;
        const sentinelSeen = waitForDiagnostics(mainUri, diags => diags.some(d => d.message.includes(sentinel)));
        const sentinelEdit = new vscode.WorkspaceEdit();
        sentinelEdit.insert(mainUri, new vscode.Position(tailLine, 0), `int v_${sentinel} = ${sentinel};
`);
        assert.ok(await vscode.workspace.applyEdit(sentinelEdit), 'sentinel edit must apply');
        await sentinelSeen;

        const finalHover = await getHoverText(mainUri, innerPos);
        assert.ok(
            finalHover.includes(symbols.innerFn),
            `Expected final hover to contain ${symbols.innerFn}, got: ${finalHover}`
        );
    });
});
