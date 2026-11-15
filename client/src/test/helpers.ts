import * as assert from 'assert';
import * as vscode from 'vscode';

/** @brief Generates a unique alphanumeric suffix for isolated test fixtures and symbols. */
export function randomSuffix(): string {
    return `${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`;
}

/** @brief Finds the AngelScript extension under any of its published ids and activates it. */
export async function activateExtension(): Promise<void> {
    const extension = vscode.extensions.getExtension('Gaftherman.angelscript-gaftherman')
        ?? vscode.extensions.getExtension('Gaftherman.angelscript')
        ?? vscode.extensions.all.find(
            candidate => candidate.packageJSON?.name === 'angelscript-gaftherman'
                || candidate.packageJSON?.name === 'angelscript'
        );
    assert.ok(extension, 'AngelScript extension must be installed in the test host');
    await extension.activate();
}

/**
 * @brief Resolves on the first diagnostics change for a URI whose current diagnostics satisfy a predicate.
 * @param uri Document whose diagnostics are watched.
 * @param accept Predicate over the diagnostics published for the document; defaults to accepting any state.
 */
export function waitForDiagnostics(
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
export async function getCompletionLabels(uri: vscode.Uri, position: vscode.Position): Promise<string[]> {
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
export async function getHoverText(uri: vscode.Uri, position: vscode.Position): Promise<string> {
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
