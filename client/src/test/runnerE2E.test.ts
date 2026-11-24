import * as assert from 'assert';
import { once } from 'events';
import * as fs from 'fs';
import * as net from 'net';
import * as path from 'path';
import * as vscode from 'vscode';
import { activateExtension, randomSuffix } from './helpers';

interface StoppedDetails {
    reason?: string;
    threadId?: number;
}

interface AdapterMessage {
    type: string;
    event?: string;
    body?: StoppedDetails & { category?: string; output?: string };
}

const fixtureDirectory = path.resolve(__dirname, '../../test-fixtures/runner');
const debugHost = '127.0.0.1';

/** @brief Captures launch events even when the adapter exits before startDebugging resolves. */
function trackLaunch(name: string) {
    const output = { stdout: '', stderr: '' };
    let session: vscode.DebugSession | undefined;
    let onStopped!: (details: StoppedDetails) => void;
    let onTerminated!: () => void;
    const stopped = new Promise<StoppedDetails>(resolve => { onStopped = resolve; });
    const terminated = new Promise<void>(resolve => { onTerminated = resolve; });
    const tracker = vscode.debug.registerDebugAdapterTrackerFactory('angelscript', {
        createDebugAdapterTracker(candidate) {
            if (candidate.name !== name) {
                return undefined;
            }
            session = candidate;
            return {
                onDidSendMessage(message: AdapterMessage) {
                    if (message.type !== 'event') {
                        return;
                    }
                    const body = message.body;
                    if (message.event === 'output' && (body?.category === 'stdout' || body?.category === 'stderr')) {
                        output[body.category] += body.output ?? '';
                    }
                    if (message.event === 'stopped') {
                        onStopped(body ?? {});
                    }
                }
            };
        }
    });
    const terminationListener = vscode.debug.onDidTerminateDebugSession(ended => {
        if (ended.id === session?.id) {
            session = undefined;
            onTerminated();
        }
    });

    return {
        get session() { return session; },
        output,
        stopped,
        terminated,
        subscriptions: vscode.Disposable.from(tracker, terminationListener),
    };
}

/** @brief Bounds an event or request wait and releases its timer when it settles. */
async function within<T>(task: PromiseLike<T>, timeoutMs: number, operation: string): Promise<T> {
    let timer!: NodeJS.Timeout;
    const timeout = new Promise<never>((_resolve, reject) => {
        timer = setTimeout(() => reject(new Error(`Timed out waiting for ${operation}`)), timeoutMs);
    });
    try {
        return await Promise.race([task, timeout]);
    } finally {
        clearTimeout(timer);
    }
}

/** @brief Lets the OS select a loopback port for the CLI's DAP listener. */
async function getDebugPort(): Promise<number> {
    const server = net.createServer();
    const listening = once(server, 'listening');
    server.listen(0, debugHost);
    await listening;
    const address = server.address();
    const closed = once(server, 'close');
    server.close();
    await closed;
    assert.ok(address && typeof address !== 'string', 'Expected a TCP listener address');
    return address.port;
}

/** @brief Checks complete output lines after reassembling the adapter's output chunks. */
function assertOutputLines(output: string, expected: readonly string[]): void {
    const lines = output.split(/\r?\n/);
    for (const line of expected) {
        assert.ok(lines.includes(line), `Expected output line ${JSON.stringify(line)} in:\n${output}`);
    }
}

suite('AngelScript runner E2E', function () {
    let folder: vscode.WorkspaceFolder;
    let launch: ReturnType<typeof trackLaunch> | undefined;
    let breakpoint: vscode.SourceBreakpoint | undefined;
    let restoreRunnerPath: (() => Thenable<void>) | undefined;

    suiteSetup(async () => {
        const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(workspaceFolder, 'Requires an open workspace folder');
        folder = workspaceFolder;
        await activateExtension();
    });

    async function start(runnerPath: string, configuration: vscode.DebugConfiguration) {
        const config = vscode.workspace.getConfiguration('angelscript', folder);
        const previous = config.inspect<string>('runner.path')?.workspaceValue;
        restoreRunnerPath = () => config.update('runner.path', previous, vscode.ConfigurationTarget.Workspace);
        await config.update('runner.path', runnerPath, vscode.ConfigurationTarget.Workspace);

        const current = trackLaunch(configuration.name);
        launch = current;
        assert.ok(await vscode.debug.startDebugging(folder, configuration), `Could not start ${configuration.name}`);
        return current;
    }

    teardown(async () => {
        try {
            if (launch?.session) {
                const stop = vscode.debug.stopDebugging(launch.session);
                await within(Promise.all([stop, launch.terminated]), 10_000, 'debug session cleanup');
            }
        } finally {
            launch?.subscriptions.dispose();
            launch = undefined;
            if (breakpoint) {
                vscode.debug.removeBreakpoints([breakpoint]);
                breakpoint = undefined;
            }
            const restore = restoreRunnerPath;
            restoreRunnerPath = undefined;
            await restore?.();
        }
    });

    suite('with angelscript-cli', function () {
        let runnerPath: string;

        suiteSetup(async function () {
            const suppliedPath = process.env.ANGELSCRIPT_CLI_PATH;
            if (suppliedPath) {
                const candidate = path.resolve(suppliedPath);
                const stat = await fs.promises.stat(candidate).catch(() => undefined);
                if (stat?.isFile()) {
                    runnerPath = candidate;
                    return;
                }
            }
            console.log('Skipping real-runner tests: set ANGELSCRIPT_CLI_PATH to an existing angelscript-cli file.');
            this.skip();
        });

        test('runs a script with arguments and captures stdout in noDebug mode', async () => {
            const current = await start(runnerPath, {
                type: 'angelscript', request: 'launch', name: 'e2e-run',
                program: path.join(fixtureDirectory, 'hello.as'), args: ['a', 'b'], noDebug: true,
            });
            await within(current.terminated, 30_000, 'noDebug session termination');
            assertOutputLines(current.output.stdout, ['total: 14', 'argument count: 2', 'argument 0: a', 'argument 1: b']);
        });

        test('stops at a source breakpoint and resumes through termination', async () => {
            const uri = vscode.Uri.file(path.join(fixtureDirectory, 'debug.as'));
            const source = await fs.promises.readFile(uri.fsPath, 'utf8');
            const line = source.split(/\r?\n/).findIndex(text => text.endsWith('// BP'));
            assert.ok(line >= 0, 'Missing // BP marker in debug.as');
            breakpoint = new vscode.SourceBreakpoint(new vscode.Location(uri, new vscode.Position(line, 0)));
            vscode.debug.addBreakpoints([breakpoint]);

            const current = await start(runnerPath, {
                type: 'angelscript', request: 'launch', name: 'e2e-debug', program: uri.fsPath,
                host: debugHost, port: await getDebugPort(),
            });
            const stop = await within(Promise.race([current.stopped, current.terminated]), 30_000, 'breakpoint stop');
            assert.ok(stop, `Session terminated before reaching the breakpoint.\n${current.output.stderr}`);
            const { reason, threadId } = stop;
            assert.strictEqual(reason, 'breakpoint');
            assert.ok(threadId && threadId > 0, 'Expected a DAP thread ID');
            const session = current.session;
            assert.ok(session, 'Expected the stopped debug session to be active');

            const trace: { stackFrames: { line: number; source?: { path?: string } }[] } =
                await session.customRequest('stackTrace', { threadId, levels: 1 });
            const frame = trace.stackFrames[0];
            assert.ok(frame?.source?.path, 'Expected a source path for the top stack frame');
            assert.strictEqual(vscode.Uri.file(frame.source.path).fsPath, uri.fsPath);
            assert.strictEqual(frame.line, line + 1);

            await session.customRequest('continue', { threadId });
            await within(current.terminated, 30_000, 'debug session termination');
            assertOutputLines(current.output.stdout, ['debug input: 6', 'debug total: 12']);
        });
    });

    test('reports a missing runner and terminates the launch', async function () {
        this.timeout(20_000);
        const missingRunner = path.join(folder.uri.fsPath, `missing-runner-${randomSuffix()}`, 'angelscript-cli');
        const current = await start(missingRunner, {
            type: 'angelscript', request: 'launch', name: 'e2e-missing-runner',
            program: path.join(fixtureDirectory, 'hello.as'), noDebug: true,
        });
        await within(current.terminated, 10_000, 'missing-runner session termination');
        const message = vscode.l10n.t('AngelScript runner not found. Set angelscript.runner.path to an executable or clear it to use angelscript-cli from PATH.');
        assertOutputLines(current.output.stderr, [message]);
    });
});
