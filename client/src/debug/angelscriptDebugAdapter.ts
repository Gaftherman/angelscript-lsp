import * as vscode from 'vscode';
import * as net from 'net';
import type { LanguageClient } from 'vscode-languageclient/node';

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
    constructor(private readonly getClient: () => LanguageClient | undefined) {}

    createDebugAdapterDescriptor(
        session: vscode.DebugSession,
        _executable: vscode.DebugAdapterExecutable | undefined
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
        const config = session.configuration;
        const workspaceConfig = vscode.workspace.getConfiguration('angelscript');
        const port = config.port ?? workspaceConfig.get<number>('debug.defaultPort', 9002);
        const host = config.host ?? '127.0.0.1';

        // Connect to debug server socket through our custom adapter session
        return new vscode.DebugAdapterInlineImplementation(
            new AngelScriptDebugAdapterSession(host, port, this.getClient)
        );
    }
}

/**
 * @brief In-process debug adapter session bridging VS Code DAP messages with the AngelScript debug server.
 */
class AngelScriptDebugAdapterSession implements vscode.DebugAdapter {
    private readonly _sendMessage = new vscode.EventEmitter<vscode.DebugProtocolMessage>();
    readonly onDidSendMessage: vscode.Event<vscode.DebugProtocolMessage> = this._sendMessage.event;

    private socket?: net.Socket;
    private rawBuffer: Buffer = Buffer.alloc(0);
    private outboundQueue: Buffer[] = [];
    private isConnected = false;

    constructor(
        private readonly host: string,
        private readonly port: number,
        private readonly getClient: () => LanguageClient | undefined
    ) {
        this.connect();
    }

    private connect(): void {
        this.socket = net.createConnection({ host: this.host, port: this.port }, () => {
            this.isConnected = true;
            if (this.socket && !this.socket.destroyed) {
                for (const packet of this.outboundQueue) {
                    this.socket.write(packet);
                }
            }
            this.outboundQueue = [];
        });

        this.socket.on('data', (chunk: Buffer) => {
            this.handleIncomingData(chunk);
        });

        this.socket.on('error', (err: Error) => {
            this._sendMessage.fire({
                seq: 0,
                type: 'event',
                event: 'output',
                body: {
                    category: 'stderr',
                    output: `${vscode.l10n.t('AngelScript Debugger connection error: {0}', err.message)}\n`
                }
            } as vscode.DebugProtocolMessage);
        });

        this.socket.on('close', () => {
            this.isConnected = false;
            this._sendMessage.fire({
                seq: 0,
                type: 'event',
                event: 'terminated'
            } as vscode.DebugProtocolMessage);
        });
    }

    handleMessage(message: vscode.DebugProtocolMessage): void {
        const msg = message as { type?: string; command?: string; arguments?: Record<string, unknown> };

        // Verify breakpoint locations with LSP server when configured
        if (msg.type === 'request' && msg.command === 'setBreakpoints') {
            void this.verifyBreakpointsAndForward(message);
            return;
        }

        this.sendToSocket(message);
    }

    private async verifyBreakpointsAndForward(message: vscode.DebugProtocolMessage): Promise<void> {
        const workspaceConfig = vscode.workspace.getConfiguration('angelscript');
        const enableVerification = workspaceConfig.get<boolean>('debug.enableBreakpointLocations', true);
        const client = this.getClient();

        const req = message as {
            arguments?: {
                source?: { path?: string };
                lines?: number[];
                breakpoints?: Array<{ line: number; column?: number }>;
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
                    const validLines = new Set<number>(locations.map(loc => loc.range.start.line + 1));
                    const verifiedLines = requestedLines.filter(line => validLines.has(line));
                    if (verifiedLines.length > 0 && req.arguments) {
                        if (req.arguments.lines) {
                            req.arguments.lines = verifiedLines;
                        }
                        if (rawBreakpoints) {
                            req.arguments.breakpoints = rawBreakpoints.filter(b => validLines.has(b.line));
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
            const bodyStart = headerEnd + 4;
            if (this.rawBuffer.length < bodyStart + contentLength) {
                break;
            }

            const bodyBuf = this.rawBuffer.subarray(bodyStart, bodyStart + contentLength);
            this.rawBuffer = this.rawBuffer.subarray(bodyStart + contentLength);

            try {
                const parsed = JSON.parse(bodyBuf.toString('utf8')) as vscode.DebugProtocolMessage;
                this._sendMessage.fire(parsed);
            } catch {
                // Ignore malformed protocol packets
            }
        }
    }

    dispose(): void {
        this.isConnected = false;
        this.outboundQueue = [];
        if (this.socket && !this.socket.destroyed) {
            this.socket.destroy();
        }
    }
}