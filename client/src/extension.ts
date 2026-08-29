import * as path from 'path';
import * as os from 'os';
import * as fs from 'fs';
import { ExtensionContext, window, workspace, env, OutputChannel, ExtensionMode } from 'vscode';
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
 * @brief Builds the command line the server is launched with from the user's `angelscript.*` settings.
 *
 * The server exposes every one of these as a CLI flag (see ServerConfig::FromArgs); without this
 * the settings declared in package.json would be inert, because the language client passes no
 * arguments of its own.
 *
 * @return Argument list for the server process.
 */
/**
 * @brief Maps `angelscript.features.*` settings onto the server's kill-switch flags.
 *
 * Every entry is enabled by default server-side, so only a setting the user turned off produces
 * an argument.
 */
const FEATURE_FLAGS: ReadonlyArray<readonly [string, string]> = [
    ['hover', '--disable-hover'],
    ['definition', '--disable-definition'],
    ['completion', '--disable-completion'],
    ['semanticTokens', '--disable-semantic-tokens'],
    ['signatureHelp', '--disable-signature-help'],
    ['predefinedLoader', '--disable-predefined-loader'],
    ['documentSymbols', '--disable-document-symbols'],
    ['workspaceSymbols', '--disable-workspace-symbols'],
    ['references', '--disable-references'],
    ['rename', '--disable-rename'],
    ['documentHighlight', '--disable-document-highlight'],
    ['foldingRange', '--disable-folding-range'],
    ['inlayHints', '--disable-inlay-hints'],
    ['codeAction', '--disable-code-action'],
    ['formatting', '--disable-formatting'],
    ['documentLink', '--disable-document-link'],
    ['implementation', '--disable-implementation'],
    ['selectionRange', '--disable-selection-range'],
    ['callHierarchy', '--disable-call-hierarchy'],
    ['typeHierarchy', '--disable-type-hierarchy'],
    ['linkedEditing', '--disable-linked-editing'],
    ['typeConversionChecks', '--disable-type-conversion-checks'],
    ['codeLens', '--disable-code-lens'],
    ['onTypeFormatting', '--disable-on-type-formatting']
];

/**
 * @brief `angelscript.engine.*` settings, in the order the server's --engine-property expects.
 *
 * These are not feature switches: each one states how the host built its AngelScript engine, and
 * several language rules are decided by that rather than by anything in the script. The setting
 * name is the asEEngineProp identifier without its asEP_ prefix, so a host author can map its own
 * SetEngineProperty calls across without translating anything.
 *
 * Every default here is the engine's own default, which is why only a `true` is passed on: a
 * property left alone should cost nothing on the command line.
 */
const ENGINE_PROPERTIES: ReadonlyArray<string> = [
    'allowUnsafeReferences',
    'privatePropAsProtected',
    'disallowGlobalVars'
];

/**
 * @brief Expands a possibly relative path into the absolute forms to pass to the server.
 *
 * The server's working directory is not the project root, so a relative entry only means anything
 * once resolved - and in a multi-root workspace it can legitimately mean one path per folder.
 */
function resolveAgainstWorkspace(entry: string): string[] {
    const trimmed = entry.trim();
    if (trimmed.length === 0) {
        return [];
    }
    if (path.isAbsolute(trimmed)) {
        return [trimmed];
    }
    return (workspace.workspaceFolders ?? []).map(folder => path.resolve(folder.uri.fsPath, trimmed));
}

function buildServerArgs(): string[] {
    const config = workspace.getConfiguration('angelscript');
    const args: string[] = [];

    // #include search paths.
    for (const entry of config.get<string[]>('searchDirectories', [])) {
        for (const resolved of resolveAgainstWorkspace(entry)) {
            args.push(`--search-dir=${resolved}`);
        }
    }

    // Predefined stubs, loaded by path. Note this is NOT the same as predefinedExtension below:
    // the extension is a suffix used while scanning the workspace, these are specific files, and
    // the two used to be conflated - a configured path was passed as the suffix, which silently
    // matched nothing.
    for (const entry of config.get<string[]>('predefinedFiles', [])) {
        for (const resolved of resolveAgainstWorkspace(entry)) {
            args.push(`--predefined-file=${resolved}`);
        }
    }

    // Migration path for the old `angelscript.predefinedFile` string setting, which promised a
    // path and delivered a suffix. Read as what it always claimed to be so existing settings start
    // working rather than silently staying broken.
    const legacyPredefined = config.get<string>('predefinedFile', '').trim();
    if (legacyPredefined.length > 0) {
        for (const resolved of resolveAgainstWorkspace(legacyPredefined)) {
            args.push(`--predefined-file=${resolved}`);
        }
    }

    const predefinedExtension = config.get<string>('predefinedExtension', '').trim();
    if (predefinedExtension.length > 0) {
        args.push(`--predefined-ext=${predefinedExtension}`);
    }

    const fileExtension = config.get<string>('fileExtension', '').trim();
    if (fileExtension.length > 0) {
        args.push(`--file-ext=${fileExtension}`);
    }

    // Templates whose initializer list is a plain repeat of their element type, as `array<T>`'s is.
    // This cannot be read from a predefined stub: a list factory is registered in C++ and the stub
    // format has no notation for one, so `optional<T>` and `array<T>` are declared identically even
    // though the compiler accepts a list for only one of them.
    for (const entry of config.get<string[]>('arrayLikeTypes', [])) {
        const name = entry.trim();
        if (name.length > 0) {
            args.push(`--array-like-type=${name}`);
        }
    }

    for (const [setting, flag] of FEATURE_FLAGS) {
        if (config.get<boolean>(`features.${setting}`, true) === false) {
            args.push(flag);
        }
    }

    for (const property of ENGINE_PROPERTIES) {
        if (config.get<boolean>(`engine.${property}`, false) === true) {
            args.push(`--engine-property=${property}=true`);
        }
    }

    // asEP_PROPERTY_ACCESSOR_MODE takes a number, not a boolean, so it is not one of
    // ENGINE_PROPERTIES above either. Passed on only when the user asked for something other than
    // this server's default, keeping an untouched setting off the command line the way the
    // booleans are.
    const accessorMode = config.get<number>('engine.propertyAccessorMode', 2);
    if (accessorMode === 2 || accessorMode === 3) {
        args.push(`--engine-property=propertyAccessorMode=${accessorMode}`);
    }

    // The host dialect. An enum rather than a boolean, so it is not one of ENGINE_PROPERTIES above
    // and needs its own line; without it the setting was declared, documented and inert.
    const engineProfile = config.get<string>('engine.profile', '').trim();
    if (engineProfile.length > 0) {
        args.push(`--engine-profile=${engineProfile}`);
    }

    const severities = config.get<Record<string, string>>('diagnosticSeverity', {});
    for (const [code, severity] of Object.entries(severities ?? {})) {
        if (code.trim().length > 0 && typeof severity === 'string' && severity.trim().length > 0) {
            args.push(`--diagnostic-severity=${code.trim()}=${severity.trim()}`);
        }
    }

    // The server localises its diagnostics; align them with the editor's display language.
    if (env.language) {
        args.push(`--locale=${env.language}`);
    }

    return args;
}

/**
 * @brief Builds and starts a language client against the settings in force right now.
 *
 * Separate from activate() because most `angelscript.*` settings become server command-line
 * arguments, and ServerOptions captures those when it is constructed. Applying changed settings
 * therefore means building a new client, not restarting the existing one.
 *
 * @param context The extension execution context.
 */
async function startClient(context: ExtensionContext): Promise<void> {
    const serverPath = getServerPath(context);

    lspOutputChannel.appendLine("--- AngelScript C++ Language Server Activation ---");
    lspOutputChannel.appendLine(`Runtime Platform Context: ${os.platform()}-${os.arch()}`);
    lspOutputChannel.appendLine(`Resolved Server Binary Path: ${serverPath}`);

    const serverArgs = buildServerArgs();
    lspOutputChannel.appendLine(`Server Arguments: ${serverArgs.join(" ") || "(none)"}`);

    const serverOptions: ServerOptions = {
        run: { command: serverPath, args: serverArgs },
        debug: { command: serverPath, args: serverArgs }
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'angelscript' }],
        synchronize: {
            configurationSection: 'angelscript',
            // Without this the server only ever learns about files the user opened: a branch
            // switch, a pull, or a generated script would leave every stale symbol in the index.
            fileEvents: workspace.createFileSystemWatcher('**/*.{as,angelscript,predefined}')
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

/**
 * @brief Activates the AngelScript Language Client extension interface handlers.
 * @param context The extension context provided by VS Code.
 */
export async function activate(context: ExtensionContext) {
    lspOutputChannel = window.createOutputChannel('AngelScript C++ Language Server');

    await startClient(context);

    // Settings that become command-line arguments cannot take effect in a running server, so the
    // client is rebuilt instead. Without this the user has to reload the whole window for a
    // setting to do anything, which reads as the setting being broken.
    context.subscriptions.push(
        workspace.onDidChangeConfiguration(async event => {
            if (!event.affectsConfiguration('angelscript')) {
                return;
            }

            lspOutputChannel.appendLine('Configuration changed; restarting the language server.');
            try {
                await client?.stop();
            } catch (error) {
                lspOutputChannel.appendLine(`Failed to stop Language Client: ${error instanceof Error ? error.message : String(error)}`);
            }
            await startClient(context);
        })
    );
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
