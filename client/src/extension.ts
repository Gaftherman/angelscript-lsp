import * as path from 'path';
import * as os from 'os';
import * as fs from 'fs';
import { ExtensionContext, window, OutputChannel, ExtensionMode } from 'vscode';
import { LanguageClient, LanguageClientOptions, ServerOptions } from 'vscode-languageclient/node';

let client: LanguageClient;
let lspOutputChannel: OutputChannel;

/**
 * @brief Resolves the absolute path of the engine binary based on runtime platform and extension mode.
 * @param context The extension execution context framework.
 * @return String representation of the target binary file location path.
 */
function getServerPath(context: ExtensionContext): string {
    const platform = os.platform();
    const architecture = os.arch();
    const isWindows = platform === 'win32';
    const binaryName = isWindows ? 'angel_lsp.exe' : 'angel_lsp';

    if (context.extensionMode === ExtensionMode.Development) {
        let devPath = context.asAbsolutePath(path.join('..', 'build', 'Debug', binaryName));
        if (!isWindows) {
            devPath = context.asAbsolutePath(path.join('..', 'build', binaryName));
        }
        if (fs.existsSync(devPath)) {
            return devPath;
        }
    }

    const platformFolder = `${platform}-${architecture}`;
    const productionPath = context.asAbsolutePath(path.join('bin', platformFolder, binaryName));

    if (fs.existsSync(productionPath)) {
        if (!isWindows) {
            try {
                fs.chmodSync(productionPath, '755');
            } catch (err) {
                // Non-blocking permission failure log tracking
            }
        }
        return productionPath;
    }

    return productionPath;
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

/**
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
