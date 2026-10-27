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
    private rawBuffer = '';

    constructor(
        private readonly host: string,
        private readonly port: number,
        private readonly getClient: () => LanguageClient | undefined
    ) {
        this.connect();
    }

    private connect(): void {
        this.socket = net.createConnection({ host: this.host, port: this.port }, () => {
            // Connected successfully to AngelScript debug host
        });

        this.socket.on('data', (chunk: Buffer) => {
            this.handleIncomingData(chunk.toString('utf8'));
        });

        this.socket.on('error', (err: Error) => {
            this._sendMessage.fire({
                seq: 0,
                type: 'event',
                event: 'output',
                body: {
                    category: 'stderr',
                    output: `AngelScript Debugger connection error: ${err.message}\n`
                }
            } as vscode.DebugProtocolMessage);
        });

        this.socket.on('close', () => {
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
            this.verifyBreakpointsAndForward(message);
            return;
        }

        this.sendToSocket(message);
    }

    private async verifyBreakpointsAndForward(message: vscode.DebugProtocolMessage): Promise<void> {
        const workspaceConfig = vscode.workspace.getConfiguration('angelscript');
        const enableVerification = workspaceConfig.get<boolean>('debug.enableBreakpointLocations', true);
        const client = this.getClient();

        if (enableVerification && client) {
            const req = message as { arguments?: { source?: { path?: string }; lines?: number[] } };
            const filePath = req.arguments?.source?.path;
            if (filePath) {
                try {
                    const uri = vscode.Uri.file(filePath).toString();
                    await client.sendRequest('textDocument/breakpointLocations', {
                        textDocument: { uri }
                    });
                } catch {
                    // Fall back to direct transmission if LSP request fails
                }
            }
        }

        this.sendToSocket(message);
    }

    private sendToSocket(message: vscode.DebugProtocolMessage): void {
        if (!this.socket || this.socket.destroyed) {
            return;
        }
        const json = JSON.stringify(message);
        const payload = `Content-Length: ${Buffer.byteLength(json, 'utf8')}\r\n\r\n${json}`;
        this.socket.write(payload, 'utf8');
    }

    private handleIncomingData(data: string): void {
        this.rawBuffer += data;
        while (true) {
            const headerEnd = this.rawBuffer.indexOf('\r\n\r\n');
            if (headerEnd === -1) {
                break;
            }

            const header = this.rawBuffer.substring(0, headerEnd);
            const match = /Content-Length:\s*(\d+)/i.exec(header);
            if (!match) {
                this.rawBuffer = this.rawBuffer.substring(headerEnd + 4);
                continue;
            }

            const contentLength = parseInt(match[1], 10);
            const bodyStart = headerEnd + 4;
            if (this.rawBuffer.length < bodyStart + contentLength) {
                break;
            }

            const bodyStr = this.rawBuffer.substr(bodyStart, contentLength);
            this.rawBuffer = this.rawBuffer.substring(bodyStart + contentLength);

            try {
                const parsed = JSON.parse(bodyStr) as vscode.DebugProtocolMessage;
                this._sendMessage.fire(parsed);
            } catch {
                // Ignore malformed protocol packets
            }
        }
    }

    dispose(): void {
        if (this.socket && !this.socket.destroyed) {
            this.socket.destroy();
        }
    }
}
