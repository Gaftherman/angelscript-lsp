import * as path from 'path';
import * as os from 'os';
import * as fs from 'fs';
import { ExtensionContext, window, OutputChannel, ExtensionMode } from 'vscode';
import { LanguageClient, LanguageClientOptions, ServerOptions } from 'vscode-languageclient/node';

let client: LanguageClient;
let lspOutputChannel: OutputChannel;

/**
 * @brief Resolves the absolute path of the engine binary based on runtime platform, architecture, and fallback paths.
 * @param context The extension execution context framework.
 * @return String representation of the target binary file location path.
 */
function getServerPath(context: ExtensionContext): string {
    const platform = os.platform();
    const architecture = os.arch();
    const isWindows = platform === 'win32';
    const binaryName = isWindows ? 'angel_lsp.exe' : 'angel_lsp';

    const candidates: string[] = [];

    // 1. Development mode paths
    if (context.extensionMode === ExtensionMode.Development) {
        candidates.push(context.asAbsolutePath(path.join('..', 'server', 'build', 'Debug', binaryName)));
        candidates.push(context.asAbsolutePath(path.join('..', 'server', 'build', 'Release', binaryName)));
        candidates.push(context.asAbsolutePath(path.join('..', 'server', 'build', binaryName)));
    }

    // 2. Primary production platform-architecture path: bin/${platform}-${architecture}/${binaryName}
    const platformFolder = `${platform}-${architecture}`;
    candidates.push(context.asAbsolutePath(path.join('bin', platformFolder, binaryName)));

    // 3. Fallback generic bin path: bin/${binaryName}
    candidates.push(context.asAbsolutePath(path.join('bin', binaryName)));

    // 4. Windows fallback architecture: bin/win32-x86/${binaryName} / bin/win32-ia32/${binaryName}
    if (isWindows) {
        candidates.push(context.asAbsolutePath(path.join('bin', 'win32-x86', binaryName)));
        candidates.push(context.asAbsolutePath(path.join('bin', 'win32-ia32', binaryName)));
    }

    // 5. Development / build fallback paths (e.g. when testing compiled extension against local build)
    candidates.push(context.asAbsolutePath(path.join('..', 'server', 'build', 'Debug', binaryName)));
    candidates.push(context.asAbsolutePath(path.join('..', 'server', 'build', 'Release', binaryName)));
    candidates.push(context.asAbsolutePath(path.join('..', 'server', 'build', binaryName)));

    for (const candidate of candidates) {
        if (fs.existsSync(candidate)) {
            if (!isWindows) {
                try {
                    fs.chmodSync(candidate, '755');
                } catch {
                    // Non-blocking permission failure log tracking
                }
            }
            return candidate;
        }
    }

    return context.asAbsolutePath(path.join('bin', platformFolder, binaryName));
}

/**
 * @brief Activates the AngelScript Language Client extension interface handlers.
 * @param context The extension context provided by VS Code.
 */
export async function activate(context: ExtensionContext) {
    const serverPath = getServerPath(context);
    
    lspOutputChannel = window.createOutputChannel('AngelScript C++ Language Server');
    lspOutputChannel.appendLine("--- AngelScript C++ Language Server Activation ---");
    lspOutputChannel.appendLine(`Runtime Platform Context: ${os.platform()}-${os.arch()}`);
    lspOutputChannel.appendLine(`Resolved Server Binary Path: ${serverPath}`);

    const serverOptions: ServerOptions = {
        run: { command: serverPath },
        debug: { command: serverPath }
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'angelscript' }],
        synchronize: {
            configurationSection: 'angelscript'
        },
        outputChannel: lspOutputChannel
    };

    client = new LanguageClient(
        'angelScriptLSP',
        'AngelScript C++ Language Server',
        serverOptions,
        clientOptions
    );

    try {        
        await client.start();
        client.onNotification("angelscript/debug", (params: { message: string }) => {
            lspOutputChannel.appendLine(`[AST Debug] ${params.message}`);
        });
    } catch (error) {
        lspOutputChannel.appendLine(`Failed to start Language Client: ${error instanceof Error ? error.message : String(error)}`);
    }
}

/**7
 * @brief Deactivates the Language Client session pipeline.
 * @return Promise token indicating completion of client teardown routines.
 */
export function deactivate(): Thenable<void> | undefined {
    if (!client)
    {
        return undefined;
    } 
    return client.stop();
}
