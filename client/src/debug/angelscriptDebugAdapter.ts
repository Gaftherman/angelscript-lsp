import * as cp from 'child_process';
import * as fs from 'fs';
import * as net from 'net';
import * as path from 'path';
import * as vscode from 'vscode';
import type { LanguageClient } from 'vscode-languageclient/node';
import { resolveRunner } from './runnerResolver';

type DebugRequest = vscode.DebugProtocolMessage & { command: string; seq: number; type: string };

/**
 * @brief Provides default debug configurations for AngelScript launch and attach targets.
 */
export class AngelScriptConfigurationProvider implements vscode.DebugConfigurationProvider {
    resolveDebugConfiguration(
        _folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration,
        _token?: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.DebugConfiguration> {
        const workspaceConfig = vscode.workspace.getConfiguration('angelscript');

        if (!config.type && !config.request && !config.name) {
            const editor = vscode.window.activeTextEditor;
            if (editor && editor.document.languageId === 'angelscript') {
                config.type = 'angelscript';
                config.name = 'AngelScript: Attach to Engine';
                config.request = 'attach';
                config.host = '127.0.0.1';
                config.port = workspaceConfig.get<number>('debug.defaultPort', 9002);
                config.protocol = workspaceConfig.get<string>('debug.defaultProtocol', 'dap');
                return config;
            }
        }

        config.host ??= '127.0.0.1';
        config.port ??= workspaceConfig.get<number>('debug.defaultPort', 9002);
        config.protocol ??= workspaceConfig.get<string>('debug.defaultProtocol', 'dap');

        return config;
    }
}

/**
 * @brief Factory creating AngelScript debug adapters communicating with the target engine.
 */
export class AngelScriptDebugAdapterDescriptorFactory implements vscode.DebugAdapterDescriptorFactory {
    /**
     * @brief Supplies the language client and the runner setting for a debug session's folder.
     * @param[in] getClient Retrieves the active language client for breakpoint verification.
     * @param[in] getRunnerPath Reads the configured external runner path for the given folder.
     */
    constructor(
        private readonly getClient: () => LanguageClient | undefined,
        private readonly getRunnerPath: (folder: vscode.WorkspaceFolder | undefined) => string
    ) {}

    /**
     * @brief Creates an inline adapter with the session's folder and runner configuration.
     * @param[in] session Debug session whose launch or attach configuration the adapter uses.
     * @param[in] _executable VS Code's default adapter executable, unused by the inline adapter.
     * @return A descriptor that communicates directly with VS Code in the extension host.
     */
    createDebugAdapterDescriptor(
        session: vscode.DebugSession,
        _executable: vscode.DebugAdapterExecutable | undefined
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
        const folder = session.workspaceFolder ?? vscode.workspace.workspaceFolders?.[0];
        return new vscode.DebugAdapterInlineImplementation(
            new AngelScriptDebugAdapterSession(
                session.configuration, this.getClient, this.getRunnerPath(folder), folder?.uri.fsPath
            )
        );
    }
}

/**
 * @brief In-process debug adapter session bridging VS Code DAP messages with the AngelScript debug server.
 */
class AngelScriptDebugAdapterSession implements vscode.DebugAdapter {
    private readonly _sendMessage = new vscode.EventEmitter<vscode.DebugProtocolMessage>();
    readonly onDidSendMessage: vscode.Event<vscode.DebugProtocolMessage> = this._sendMessage.event;

    private readonly host: string;
    private readonly port: number;
    private readonly runner?: ReturnType<typeof resolveRunner>;
    private runnerTerminated = false;
    private socket?: net.Socket;
    private spawnedProcess?: cp.ChildProcess;
    private rawBuffer: Buffer = Buffer.alloc(0);
    private outboundQueue: Buffer[] = [];
    private isConnected = false;
    private retryTimeout?: NodeJS.Timeout;

    constructor(
        private readonly config: vscode.DebugConfiguration,
        private readonly getClient: () => LanguageClient | undefined,
        runnerSetting: string,
        private readonly workspacePath: string | undefined
    ) {
        const workspaceConfig = vscode.workspace.getConfiguration('angelscript');
        this.host = config.host ?? '127.0.0.1';
        this.port = config.port ?? workspaceConfig.get<number>('debug.defaultPort', 9002);

        if (config.request === 'launch') {
            if (config.executable) {
                this.spawnProcess(config.executable, config.args ?? [], config.cwd);
            } else if (config.program) {
                this.runner = resolveRunner(runnerSetting, {
                    env: process.env,
                    platform: process.platform,
                    fileExists: candidate => {
                        // Windows batch wrappers require a shell, while the debugger launches executables directly.
                        if (process.platform === 'win32' && /\.(bat|cmd)$/i.test(candidate)) {
                            return false;
                        }
                        try {
                            fs.accessSync(candidate, fs.constants.X_OK);
                            return fs.statSync(candidate).isFile();
                        } catch {
                            return false;
                        }
                    }
                });
                return;
            }
        }
        this.connectWithRetry(10, 500);
    }

    private spawnProcess(executable: string, args: string[], cwd: string | undefined): cp.ChildProcess | undefined {
        try {
            const child = cp.spawn(executable, args, { cwd, detached: false, shell: false });
            this.spawnedProcess = child;
            child.stdout?.on('data', (chunk: Buffer) => this.writeOutput('stdout', chunk.toString('utf8')));
            child.stderr?.on('data', (chunk: Buffer) => this.writeOutput('stderr', chunk.toString('utf8')));
            child.on('exit', (code, signal) => {
                this.writeOutput('console', `${vscode.l10n.t('Process exited with code {0} (signal: {1})',
                    code ?? 0, signal ?? vscode.l10n.t('none'))}\n`);
            });
            return child;
        } catch (err: unknown) {
            this.writeLaunchError(executable, err);
            return undefined;
        }
    }

    private launchRunner(runnerPath: string, initialize?: DebugRequest): void {
        const args: string[] = initialize ? ['--dap-port', String(this.port), '--dap-host', this.host] : [];
        args.push(this.config.program, ...(this.config.args ?? []));

        const executable = path.resolve(runnerPath);
        const child = this.spawnProcess(executable, args, this.config.cwd || this.workspacePath);
        if (!child) {
            if (initialize) {
                this.sendLocalResponse(initialize, {});
            }
            this.terminateRunner();
            return;
        }

        if (initialize) {
            child.once('spawn', () => {
                if (this.runnerTerminated) {
                    return;
                }
                try {
                    this.connectWithRetry(10, 500);
                    this.sendToSocket(initialize);
                } catch (error: unknown) {
                    this.writeConnectionError(error);
                    this.sendLocalResponse(initialize, {});
                    this.terminateRunner();
                }
            });
        }
        child.on('error', error => {
            this.writeLaunchError(executable, error);
            if (initialize && child.pid === undefined) {
                this.sendLocalResponse(initialize, {});
            }
        });
        child.once('close', () => {
            this.spawnedProcess = undefined;
            if (!this.isConnected) {
                this.terminateRunner();
            }
        });
    }

    private writeOutput(category: 'stdout' | 'stderr' | 'console', output: string): void {
        this._sendMessage.fire({ seq: 0, type: 'event', event: 'output', body: { category, output } } as vscode.DebugProtocolMessage);
    }

    private writeLaunchError(executable: string, error: unknown): void {
        const message = error instanceof Error ? error.message : String(error);
        this.writeOutput('stderr', `${vscode.l10n.t('Failed to launch executable "{0}": {1}', executable, message)}\n`);
    }

    private writeConnectionError(error: unknown): void {
        const message = error instanceof Error ? error.message : String(error);
        this.writeOutput('stderr', `${vscode.l10n.t('AngelScript Debugger connection error: {0}', message)}\n`);
    }

    private terminateRunner(): void {
        if (!this.runnerTerminated) {
            this.runnerTerminated = true;
            this._sendMessage.fire({ seq: 0, type: 'event', event: 'terminated' } as vscode.DebugProtocolMessage);
        }
    }

    private connectWithRetry(retriesLeft: number, delayMs: number): void {
        if (this.runner && this.runnerTerminated) {
            return;
        }
        const socket = this.socket = net.createConnection({ host: this.host, port: this.port }, () => {
            this.isConnected = true;
            if (this.socket && !this.socket.destroyed) {
                for (const packet of this.outboundQueue) {
                    this.socket.write(packet);
                }
            }
            this.outboundQueue = [];
        });

        socket.on('data', (chunk: Buffer) => {
            this.handleIncomingData(chunk);
        });

        socket.on('error', (err: Error) => {
            if (this.socket !== socket) {
                return;
            }
            if (!this.isConnected && retriesLeft > 0) {
                if (this.socket) {
                    this.socket.destroy();
                    this.socket = undefined;
                }
                this.retryTimeout = setTimeout(() => {
                    this.connectWithRetry(retriesLeft - 1, delayMs);
                }, delayMs);
                return;
            }

            this.writeConnectionError(err);
        });

        socket.on('close', () => {
            // A failed attempt that was replaced by a retry must not end the session.
            if (this.socket !== socket) {
                return;
            }
            if (this.runner) {
                this.isConnected = false;
                this.terminateRunner();
                return;
            }
            this.isConnected = false;
            this._sendMessage.fire({
                seq: 0,
                type: 'event',
                event: 'terminated'
            } as vscode.DebugProtocolMessage);
        });
    }

    handleMessage(message: vscode.DebugProtocolMessage): void {
        const msg = message as DebugRequest;

        if (this.runner && msg.type === 'request') {
            if (this.runnerTerminated || 'error' in this.runner || this.config.noDebug) {
                this.handleLocalRunnerRequest(msg);
                return;
            }
            if (msg.command === 'initialize') {
                this.launchRunner(this.runner.path, msg);
                return;
            }
        }

        // Verify breakpoint locations with LSP server when configured
        if (msg.type === 'request' && msg.command === 'setBreakpoints') {
            void this.verifyBreakpointsAndForward(message);
            return;
        }

        this.sendToSocket(message);
    }

    private handleLocalRunnerRequest(request: DebugRequest): void {
        switch (request.command) {
            case 'initialize':
                this.sendLocalResponse(request, {});
                break;
            case 'launch':
                this.sendLocalResponse(request);
                if (!this.runner || this.runnerTerminated) {
                    return;
                }
                if ('path' in this.runner) {
                    this.launchRunner(this.runner.path);
                } else {
                    const message = vscode.l10n.t('AngelScript runner not found. Set angelscript.runner.path to an executable or clear it to use angelscript-cli from PATH.');
                    this.writeOutput('stderr', `${message}\n`);
                    void this.showRunnerSettings(message);
                    this.terminateRunner();
                }
                break;
            case 'threads':
                this.sendLocalResponse(request, { threads: [] });
                break;
            case 'disconnect':
                this.sendLocalResponse(request);
                break;
            default:
                this.sendLocalResponse(request, undefined, false);
                break;
        }
    }

    private sendLocalResponse(request: DebugRequest, body?: object, success = true): void {
        this._sendMessage.fire({
            seq: 0, type: 'response', request_seq: request.seq, command: request.command, success, body
        } as vscode.DebugProtocolMessage);
    }

    private async showRunnerSettings(message: string): Promise<void> {
        try {
            const action = vscode.l10n.t('Open Settings');
            if (await vscode.window.showErrorMessage(message, action) === action) {
                await vscode.commands.executeCommand('workbench.action.openSettings', '@id:angelscript.runner.path');
            }
        } catch {
            // The launch failure is already visible in the Debug Console.
        }
    }

    private async verifyBreakpointsAndForward(message: vscode.DebugProtocolMessage): Promise<void> {
        const workspaceConfig = vscode.workspace.getConfiguration('angelscript');
        const enableVerification = workspaceConfig.get<boolean>('debug.enableBreakpointLocations', true);
        const client = this.getClient();

        const req = message as {
            arguments?: {
                source?: { path?: string };
                lines?: number[];
                breakpoints?: Array<{
                    line: number;
                    column?: number;
                    condition?: string;
                    hitCondition?: string;
                    logMessage?: string;
                }>;
            };
        };
        const filePath = req.arguments?.source?.path;
        const rawBreakpoints = req.arguments?.breakpoints;
        const requestedLines = req.arguments?.lines ?? rawBreakpoints?.map(b => b.line) ?? [];

        if (enableVerification && client && filePath && requestedLines.length > 0) {
            try {
                const uri = vscode.Uri.file(filePath).toString();
                const minLine = Math.max(0, Math.min(...requestedLines) - 1);
                const maxLine = Math.max(0, Math.max(...requestedLines));

                const locations = await client.sendRequest<{ range: vscode.Range }[]>(
                    'textDocument/breakpointLocations',
                    {
                        textDocument: { uri },
                        range: {
                            start: { line: minLine, character: 0 },
                            end: { line: maxLine, character: 0 }
                        }
                    }
                );

                if (Array.isArray(locations) && locations.length > 0) {
                    const sortedValidLines = Array.from(new Set(locations.map(loc => loc.range.start.line + 1))).sort((a, b) => a - b);
                    const validSet = new Set<number>(sortedValidLines);
                    if (rawBreakpoints) {
                        for (const bp of rawBreakpoints) {
                            if (!validSet.has(bp.line)) {
                                let low = 0;
                                let high = sortedValidLines.length - 1;
                                let best = -1;
                                while (low <= high) {
                                    const mid = (low + high) >> 1;
                                    if (sortedValidLines[mid] >= bp.line) {
                                        best = sortedValidLines[mid];
                                        high = mid - 1;
                                    } else {
                                        low = mid + 1;
                                    }
                                }
                                if (best !== -1) {
                                    bp.line = best;
                                }
                            }
                        }
                        if (req.arguments?.lines) {
                            req.arguments.lines = rawBreakpoints.map(b => b.line);
                        }
                    }
                }
            } catch {
                // Fall back to direct transmission if LSP request fails
            }
        }

        this.sendToSocket(message);
    }

    private sendToSocket(message: vscode.DebugProtocolMessage): void {
        const json = JSON.stringify(message);
        const bodyBuffer = Buffer.from(json, 'utf8');
        const header = `Content-Length: ${bodyBuffer.length}\r\n\r\n`;
        const payload = Buffer.concat([Buffer.from(header, 'utf8'), bodyBuffer]);

        if (this.isConnected && this.socket && !this.socket.destroyed) {
            this.socket.write(payload);
        } else {
            this.outboundQueue.push(payload);
        }
    }

    private static readonly MAX_DAP_PAYLOAD_SIZE = 16 * 1024 * 1024; // 16 MB envelope

    private handleIncomingData(chunk: Buffer): void {
        this.rawBuffer = Buffer.concat([this.rawBuffer, chunk]);
        const delimiter = Buffer.from('\r\n\r\n', 'utf8');

        while (true) {
            const headerEnd = this.rawBuffer.indexOf(delimiter);
            if (headerEnd === -1) {
                break;
            }

            const header = this.rawBuffer.subarray(0, headerEnd).toString('utf8');
            const match = /Content-Length:\s*(\d+)/i.exec(header);
            if (!match) {
                this.rawBuffer = this.rawBuffer.subarray(headerEnd + 4);
                continue;
            }

            const contentLength = parseInt(match[1], 10);
            if (isNaN(contentLength) || contentLength < 0 || contentLength > AngelScriptDebugAdapterSession.MAX_DAP_PAYLOAD_SIZE) {
                this.rawBuffer = this.rawBuffer.subarray(headerEnd + 4);
                continue;
            }

            const bodyStart = headerEnd + 4;
            if (this.rawBuffer.length < bodyStart + contentLength) {
                break;
            }

            const bodyBuf = this.rawBuffer.subarray(bodyStart, bodyStart + contentLength);
            this.rawBuffer = this.rawBuffer.length > bodyStart + contentLength
                ? Buffer.from(this.rawBuffer.subarray(bodyStart + contentLength))
                : Buffer.alloc(0);

            try {
                const parsed = JSON.parse(bodyBuf.toString('utf8')) as vscode.DebugProtocolMessage;
                this._sendMessage.fire(parsed);
            } catch {
                // Ignore malformed protocol packets
            }
        }
    }

    dispose(): void {
        this.runnerTerminated = true;
        this.isConnected = false;
        this.outboundQueue = [];
        if (this.retryTimeout) {
            clearTimeout(this.retryTimeout);
            this.retryTimeout = undefined;
        }
        if (this.socket && !this.socket.destroyed) {
            this.socket.destroy();
        }
        if (this.spawnedProcess && !this.spawnedProcess.killed) {
            this.spawnedProcess.kill();
        }
    }
}
