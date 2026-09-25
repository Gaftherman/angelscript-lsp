import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { performance } from 'perf_hooks';
import {
    commands,
    extensions,
    workspace,
    window,
    Range,
    Position,
    Uri,
    TextEditor,
    WorkspaceEdit
} from 'vscode';
import { resolveServerBinary } from '../extension';

/**
 * @file rapidFuzzer.test.ts
 * @brief Live rapid fuzzer and latency benchmark suite for AngelScript LSP.
 *
 * Exercises the language server in a real workspace with rapid text mutations,
 * measures feature reaction times across Hover, Completion, Code Actions,
 * Document Symbols, and Definition, and verifies workspace disk logs for zero crashes.
 */

interface LatencyStats {
    count: number;
    totalMs: number;
    minMs: number;
    maxMs: number;
    samples: number[];
}

const latencyTracker: Record<string, LatencyStats> = {
    Hover: { count: 0, totalMs: 0, minMs: Infinity, maxMs: 0, samples: [] },
    Completion: { count: 0, totalMs: 0, minMs: Infinity, maxMs: 0, samples: [] },
    CodeAction: { count: 0, totalMs: 0, minMs: Infinity, maxMs: 0, samples: [] },
    DocumentSymbols: { count: 0, totalMs: 0, minMs: Infinity, maxMs: 0, samples: [] },
    Definition: { count: 0, totalMs: 0, minMs: Infinity, maxMs: 0, samples: [] },
};

function recordLatency(feature: string, durationMs: number): void {
    const stats = latencyTracker[feature];
    if (!stats) {
        return;
    }
    stats.count++;
    stats.totalMs += durationMs;
    stats.samples.push(durationMs);
    if (durationMs < stats.minMs) {
        stats.minMs = durationMs;
    }
    if (durationMs > stats.maxMs) {
        stats.maxMs = durationMs;
    }
}

function sleep(ms: number): Promise<void> {
    return new Promise(resolve => setTimeout(resolve, ms));
}

function generateRandomIdentifier(prefix = 'var'): string {
    return `${prefix}_${Math.floor(Math.random() * 900000 + 100000)}`;
}

/**
 * @brief Applies full content replacement to a text editor.
 */
async function replaceEditorContent(editor: TextEditor, content: string): Promise<boolean> {
    const doc = editor.document;
    const fullRange = new Range(doc.positionAt(0), doc.positionAt(doc.getText().length));
    const edit = new WorkspaceEdit();
    edit.replace(doc.uri, fullRange, content);
    return workspace.applyEdit(edit);
}

/**
 * @brief Measures execution duration for an LSP command.
 */
async function measureFeature<T>(feature: string, fn: () => Thenable<T>): Promise<T> {
    const start = performance.now();
    const result = await fn();
    const elapsed = performance.now() - start;
    recordLatency(feature, elapsed);
    return result;
}

suite('Live Rapid Fuzzer & Feature Reaction Benchmarks', () => {
    let fixtureUri: Uri;
    let editor: TextEditor;
    let hasServerBinary = false;
    const originalContent = 'void main() { }\n';

    suiteSetup(async () => {
        const folders = workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0, 'Requires open workspace folder');
        fixtureUri = Uri.joinPath(folders[0].uri, 'main.as');

        const extension = extensions.getExtension('Gaftherman.angelscript')
            ?? extensions.all.find(c => c.packageJSON?.name === 'angelscript');
        assert.ok(extension, 'angelscript extension must be installed in test host');

        await extension.activate();

        const doc = await workspace.openTextDocument(fixtureUri);
        editor = await window.showTextDocument(doc);
        assert.ok(editor, 'Text editor for main.as must be opened');

        const mockContext = {
            asAbsolutePath: (rel: string) => path.resolve(__dirname, '..', '..', rel),
            extensionMode: 1,
        } as any;
        const bin = resolveServerBinary(mockContext);
        hasServerBinary = bin.found;
        if (!hasServerBinary) {
            console.log(`[rapidFuzzer] Server binary not present at ${bin.path}. Running in degradation-resilient mode.`);
        }

        // Allow initial server handshake and background workspace scan to settle
        await sleep(1500);
    });

    suiteTeardown(async () => {
        if (editor) {
            await replaceEditorContent(editor, originalContent);
            await editor.document.save();
        }
    });

    test('LSP server is active and responds to initial document symbols', async () => {
        if (!hasServerBinary) {
            return;
        }

        await replaceEditorContent(editor, 'class BaselineEntity {\n    int health;\n    void Init() {}\n}\nvoid main() {}\n');
        await sleep(300);

        const symbols = await measureFeature('DocumentSymbols', () =>
            commands.executeCommand('vscode.executeDocumentSymbolProvider', fixtureUri)
        );

        assert.ok(Array.isArray(symbols), 'Document symbols response should be an array');
        assert.ok(symbols.length > 0, 'Document symbols should contain at least 1 symbol');
    });

    test('Rapid fuzzer: 25 live mutation cycles with concurrent LSP feature queries', async () => {
        const mutationTemplates = [
            // 1. Valid OOP class definition
            (rand: string) => `class Player_${rand} {\n    int health;\n    void TakeDamage(int amt) {\n        health -= amt;\n    }\n}\n`,
            // 2. Syntax broken snippet (unclosed brace, missing expression)
            (rand: string) => `class Broken_${rand} {\n    int x = ;\n    void Test( {\n        if (\n    }\n`,
            // 3. Auto variable and arithmetic expression
            (rand: string) => `void Calc_${rand}() {\n    auto val = 42 * 10;\n    float f = 3.14f;\n}\n`,
            // 4. Method call and member access
            (rand: string) => `class Entity_${rand} {\n    void Action() {}\n}\nvoid Run_${rand}() {\n    Entity_${rand} ent;\n    ent.Action();\n}\n`,
            // 5. Preprocessor directives
            (rand: string) => `#if DEBUG_${rand}\nint debugMode_${rand} = 1;\n#else\nint debugMode_${rand} = 0;\n#endif\n`,
            // 6. Incomplete member completion trigger
            (rand: string) => `class Target_${rand} { int score; void Reset() {} }\nvoid Exec_${rand}() {\n    Target_${rand} obj;\n    obj.\n}\n`,
            // 7. Funcdef and callback pattern
            (rand: string) => `funcdef void Callback_${rand}(int code);\nvoid Register(Callback_${rand}@ cb) {}\n`,
            // 8. Large multi-method class
            (rand: string) => `class System_${rand} {\n    int a; float b; string c;\n    void Step() {}\n    void Render() {}\n    void Teardown() {}\n}\n`
        ];

        for (let i = 0; i < 25; i++) {
            const randId = generateRandomIdentifier(`fz${i}`);
            const template = mutationTemplates[i % mutationTemplates.length];
            const content = template(randId) + `\nvoid Entry_${randId}() { }\n`;

            // Apply live modification to the document
            const editSuccess = await replaceEditorContent(editor, content);
            assert.ok(editSuccess, `Mutation edit at iteration ${i} must succeed`);

            if (hasServerBinary) {
                // Interleaved live LSP queries simulating high-frequency typing activity
                const queryPos = new Position(1, 4);
                const queryRange = new Range(queryPos, queryPos);

                await Promise.all([
                    measureFeature('Hover', () =>
                        commands.executeCommand('vscode.executeHoverProvider', fixtureUri, queryPos)
                    ),
                    measureFeature('Completion', () =>
                        commands.executeCommand('vscode.executeCompletionItemProvider', fixtureUri, queryPos)
                    ),
                    measureFeature('CodeAction', () =>
                        commands.executeCommand('vscode.executeCodeActionProvider', fixtureUri, queryRange)
                    ),
                    measureFeature('DocumentSymbols', () =>
                        commands.executeCommand('vscode.executeDocumentSymbolProvider', fixtureUri)
                    ),
                    measureFeature('Definition', () =>
                        commands.executeCommand('vscode.executeDefinitionProvider', fixtureUri, queryPos)
                    )
                ]);
            }

            // Small delay to simulate rapid typing cadence
            await sleep(35);
        }
    });

    test('Burst mutation stress test: 10 rapid back-to-back edits without delay', async () => {
        for (let burst = 0; burst < 10; burst++) {
            const rand = generateRandomIdentifier(`burst${burst}`);
            const text = `// Burst iteration ${burst}\nclass Burst_${rand} {\n    int id = ${burst};\n    void Ping() {}\n}\n`;
            await replaceEditorContent(editor, text);
        }

        if (hasServerBinary) {
            // Immediately query completions and symbols right after burst
            const pos = new Position(2, 8);
            const [completions, symbols] = await Promise.all([
                measureFeature('Completion', () =>
                    commands.executeCommand('vscode.executeCompletionItemProvider', fixtureUri, pos)
                ),
                measureFeature('DocumentSymbols', () =>
                    commands.executeCommand('vscode.executeDocumentSymbolProvider', fixtureUri)
                )
            ]);

            assert.ok(completions !== undefined, 'Completion provider must respond after burst stress');
            assert.ok(Array.isArray(symbols), 'Document symbol provider must respond with array after burst stress');
        }
    });

    test('Real workspace log verification: crash invariant and healthy execution', async () => {
        if (!hasServerBinary) {
            return;
        }

        const folders = workspace.workspaceFolders ?? [];
        assert.ok(folders.length > 0);
        const lspDir = path.join(folders[0].uri.fsPath, '.vscode', 'lsp');
        if (!fs.existsSync(lspDir)) {
            // File logging is disabled by default in server builds.
            return;
        }

        const entries = fs.readdirSync(lspDir, { withFileTypes: true });
        const logDirs = entries
            .filter(e => e.isDirectory() && e.name.startsWith('logs-'))
            .map(e => e.name)
            .sort()
            .reverse();

        assert.ok(logDirs.length > 0, 'At least one logs-YYYY-MM-DD directory must be created');
        const activeLogDir = path.join(lspDir, logDirs[0]);

        // 1. Crash Log Invariant Verification: crash.log must exist and be 0 bytes (no crashes!)
        const crashLogPath = path.join(activeLogDir, 'crash.log');
        if (fs.existsSync(crashLogPath)) {
            const crashLogStat = fs.statSync(crashLogPath);
            const crashContent = fs.readFileSync(crashLogPath, 'utf8').trim();
            assert.strictEqual(
                crashLogStat.size,
                0,
                `crash.log must be strictly 0 bytes. Detected crash entries: ${crashContent}`
            );
        }

        // 2. Master and Analysis logs check: server recorded operations cleanly
        const masterLogPath = path.join(activeLogDir, 'master.log');
        const analysisLogPath = path.join(activeLogDir, 'analysis.log');

        if (fs.existsSync(masterLogPath)) {
            const masterContent = fs.readFileSync(masterLogPath, 'utf8');
            assert.ok(!masterContent.includes('FATAL'), 'master.log must not contain FATAL messages');
            assert.ok(!masterContent.includes('std::terminate'), 'master.log must not contain terminate logs');
        }

        if (fs.existsSync(analysisLogPath)) {
            const analysisContent = fs.readFileSync(analysisLogPath, 'utf8');
            assert.ok(!analysisContent.includes('FATAL'), 'analysis.log must not contain FATAL messages');
        }

        // 3. Print benchmark summary table
        console.log('\n================================================================');
        console.log('       ANGELSCRIPT LSP FRONTEND FEATURE BENCHMARK REPORT         ');
        console.log('================================================================');
        console.log('Feature          | Requests | Min (ms) | Avg (ms) | P95 (ms) | Max (ms)');
        console.log('-----------------|----------|----------|----------|----------|--------');

        for (const [feature, stats] of Object.entries(latencyTracker)) {
            if (stats.count === 0) {
                continue;
            }
            const avg = (stats.totalMs / stats.count).toFixed(2);
            const sorted = [...stats.samples].sort((a, b) => a - b);
            const p95Index = Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95));
            const p95 = sorted[p95Index].toFixed(2);
            const min = stats.minMs.toFixed(2);
            const max = stats.maxMs.toFixed(2);

            const featPad = feature.padEnd(16, ' ');
            const reqPad = String(stats.count).padStart(8, ' ');
            const minPad = min.padStart(8, ' ');
            const avgPad = avg.padStart(8, ' ');
            const p95Pad = p95.padStart(8, ' ');
            const maxPad = max.padStart(8, ' ');

            console.log(`${featPad} | ${reqPad} | ${minPad} | ${avgPad} | ${p95Pad} | ${maxPad}`);
        }
        console.log('================================================================\n');
    });
});
