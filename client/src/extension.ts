import * as path from 'path';
import * as os from 'os';
import * as fs from 'fs';
import {
    ExtensionContext, window, workspace, env, commands, OutputChannel, ExtensionMode,
    StatusBarAlignment, StatusBarItem, ThemeColor
} from 'vscode';
import {
    LanguageClient, LanguageClientOptions, ServerOptions, ErrorHandler, ErrorAction, CloseAction
} from 'vscode-languageclient/node';

let client: LanguageClient;
let lspOutputChannel: OutputChannel;
let statusBarItem: StatusBarItem;

/** @brief Command that reveals the server log. The status bar item and every error offer it. */
const SHOW_LOG_COMMAND = 'angelscript.showServerLog';

/** @brief Command that stops the running server and starts a fresh one with current settings. */
const RESTART_COMMAND = 'angelscript.restartServer';

/** @brief Wording of the button on every failure notification. */
const SHOW_LOG_ACTION = 'Show Log';

/**
 * @brief How many unexpected server exits are absorbed before the user is told and it stays down.
 *
 * A crash loop is worth reporting once, not four times a second. The count matches the language
 * client's own default, so this changes what the user is told rather than how hard it tries.
 */
const MAX_SILENT_RESTARTS = 4;

/** @brief Server exits absorbed so far, reset on every successful start. */
let unexpectedExits = 0;

/**
 * @brief Reports the server's state where the user can see it without opening a panel.
 *
 * The extension had no surface at all: a server that never started and a server working normally
 * looked identical from the editor, because the only difference was a line in an output channel
 * nobody has open. Every state here is clickable and reveals that channel.
 */
function setStatus(state: 'starting' | 'running' | 'failed', tooltip: string): void {
    if (!statusBarItem) {
        return;
    }

    const label = { starting: '$(sync~spin) AngelScript', running: '$(check) AngelScript', failed: '$(error) AngelScript' };
    statusBarItem.text = label[state];
    statusBarItem.tooltip = `${tooltip}\nClick to open the server log.`;
    statusBarItem.backgroundColor = state === 'failed'
        ? new ThemeColor('statusBarItem.errorBackground')
        : undefined;
    statusBarItem.show();
}

/**
 * @brief Tells the user the server is not running, in the editor rather than in a log.
 *
 * @param summary One line, and the only part most users will read.
 * @param detail  What was tried, written to the log in full.
 */
function reportFailure(summary: string, detail?: string): void {
    lspOutputChannel.appendLine(summary);
    if (detail) {
        lspOutputChannel.appendLine(detail);
    }

    setStatus('failed', summary);

    void window.showErrorMessage(`AngelScript: ${summary}`, SHOW_LOG_ACTION).then(choice => {
        if (choice === SHOW_LOG_ACTION) {
            lspOutputChannel.show(true);
        }
    });
}

/** @brief Where the server binary was found, or every place that was looked when it was not. */
interface ServerBinary {
    /** @brief The binary to launch. When `found` is false this is where one was expected. */
    path: string;
    found: boolean;
    /** @brief Every candidate, in the order they were tried. Only worth printing on a failure. */
    searched: string[];
}

/**
 * @brief Resolves the absolute path of the engine binary based on runtime platform, architecture, and fallback paths.
 * @param context The extension execution context framework.
 * @return The binary, or the expected location and the full search when there is none.
 */
function resolveServerBinary(context: ExtensionContext): ServerBinary {
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
            return { path: candidate, found: true, searched: candidates };
        }
    }

    // The primary production path is the honest thing to name when nothing exists: it is where a
    // packaged extension is supposed to carry the binary for this platform.
    return {
        path: context.asAbsolutePath(path.join('bin', platformFolder, binaryName)),
        found: false,
        searched: candidates
    };
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
    for (const entry of config.get<string[]>('exclude', [])) {
        if (entry.trim().length > 0) {
            args.push(`--exclude=${entry}`);
        }
    }

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

    // Opt-in diagnostics. Not a feature switch and not an engine option: a rule that is right for
    // a workspace whose declarations are complete and wrong for one whose host registers types in
    // C++. Passed on only when asked for, so an untouched setting stays off the command line.
    if (config.get<boolean>('diagnostics.reportUnknownTypes', true) === false) {
        args.push('--no-report-unknown-types');
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

    // Where a block's opening brace goes. Only "kr" moves it; anything else, a typo included, is
    // the Allman default, so a misspelling reformats nothing unexpectedly. A list or a lambda body
    // keeps its brace on the line under either style - that is not a matter of taste.
    const braceStyle = config.get<string>('format.braceStyle', '').trim();
    if (braceStyle.length > 0) {
        args.push(`--format-brace-style=${braceStyle}`);
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
    const server = resolveServerBinary(context);

    lspOutputChannel.appendLine("--- AngelScript C++ Language Server Activation ---");
    lspOutputChannel.appendLine(`Runtime Platform Context: ${os.platform()}-${os.arch()}`);
    lspOutputChannel.appendLine(`Resolved Server Binary Path: ${server.path}`);

    // Checked before launching rather than left to the spawn. A missing binary is the one failure
    // with a specific, actionable cause - the extension carries no build for this platform, or a
    // source checkout has not been built - and letting it arrive as a bare ENOENT threw that away.
    if (!server.found) {
        reportFailure(
            `no language server binary for ${os.platform()}-${os.arch()}. Editor features are unavailable.`,
            `Looked in:\n  ${server.searched.join('\n  ')}`);
        return;
    }

    setStatus('starting', 'Starting the AngelScript language server.');

    const serverArgs = buildServerArgs();
    lspOutputChannel.appendLine(`Server Arguments: ${serverArgs.join(" ") || "(none)"}`);

    const serverOptions: ServerOptions = {
        run: { command: server.path, args: serverArgs },
        debug: { command: server.path, args: serverArgs }
    };

    // Without one of these the client uses its default, which restarts a few times and then stops
    // for good without a word. A server that has given up looks exactly like one with nothing to
    // say, so the state is reported once, at the point it becomes permanent.
    const errorHandler: ErrorHandler = {
        error: (error, _message, count) => {
            if (count !== undefined && count > 3) {
                reportFailure(
                    'the language server connection keeps failing, so it has been shut down.',
                    error.message);
                return { action: ErrorAction.Shutdown, handled: true };
            }
            lspOutputChannel.appendLine(`Language server connection error: ${error.message}`);
            return { action: ErrorAction.Continue, handled: true };
        },
        closed: () => {
            unexpectedExits++;
            if (unexpectedExits > MAX_SILENT_RESTARTS) {
                reportFailure(
                    `the language server stopped ${unexpectedExits} times and will not be restarted again.`);
                return { action: CloseAction.DoNotRestart, handled: true };
            }
            lspOutputChannel.appendLine(
                `Language server exited unexpectedly (${unexpectedExits}/${MAX_SILENT_RESTARTS}); restarting.`);
            return { action: CloseAction.Restart, handled: true };
        }
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'angelscript' }],
        synchronize: {
            configurationSection: 'angelscript',
            // Without this the server only ever learns about files the user opened: a branch
            // switch, a pull, or a generated script would leave every stale symbol in the index.
            // angelscript.exclude does NOT apply here: createFileSystemWatcher takes a single
            // include glob and no exclusions, so a build tree full of .as files is still watched.
            // VS Code's own `files.watcherExclude` governs that, and it already excludes .git and
            // node_modules by default. Said plainly because the server-side scans ARE pruned, and
            // assuming the watcher followed them would be the natural mistake to make.
            fileEvents: workspace.createFileSystemWatcher('**/*.{as,angelscript,predefined}')
        },
        outputChannel: lspOutputChannel,
        errorHandler
    };

    client = new LanguageClient(
        'angelScriptLSP',
        'AngelScript C++ Language Server',
        serverOptions,
        clientOptions
    );

    try {
        await client.start();
        unexpectedExits = 0;
        setStatus('running', 'The AngelScript language server is running.');
        client.onNotification("angelscript/debug", (params: { message: string }) => {
            lspOutputChannel.appendLine(`[AST Debug] ${params.message}`);
        });
    } catch (error) {
        reportFailure(
            'the language server failed to start. Editor features are unavailable.',
            `Failed to start Language Client: ${error instanceof Error ? error.stack ?? error.message : String(error)}`);
    }
}

/**
 * @brief Activates the AngelScript Language Client extension interface handlers.
 * @param context The extension context provided by VS Code.
 */
export async function activate(context: ExtensionContext) {
    lspOutputChannel = window.createOutputChannel('AngelScript C++ Language Server');
    context.subscriptions.push(lspOutputChannel);

    context.subscriptions.push(
        commands.registerCommand(SHOW_LOG_COMMAND, () => lspOutputChannel.show(true)));

    context.subscriptions.push(
        commands.registerCommand(RESTART_COMMAND, () => restartClient(context, 'Restart requested from the command palette.')));

    statusBarItem = window.createStatusBarItem(StatusBarAlignment.Right, 100);
    statusBarItem.command = SHOW_LOG_COMMAND;
    context.subscriptions.push(statusBarItem);

    await startClient(context);

    // Settings that become command-line arguments cannot take effect in a running server, so the
    // client is rebuilt instead. Without this the user has to reload the whole window for a
    // setting to do anything, which reads as the setting being broken.
    context.subscriptions.push(
        workspace.onDidChangeConfiguration(async event => {
            if (!event.affectsConfiguration('angelscript')) {
                return;
            }

            await restartClient(context, 'Configuration changed; restarting the language server.');
        })
    );
}

/**
 * @brief Stops the running client, if any, and builds a fresh one.
 *
 * Server-side this needs nothing: every setting that matters is a command-line argument captured
 * when ServerOptions is constructed, so a new client *is* the restart. That is also why the
 * command lives here rather than behind workspace/executeCommand - a request handled by the
 * process being restarted cannot outlive the restart.
 *
 * @param context The extension execution context.
 * @param reason Written to the log, so a restart is never a silent gap in it.
 */
async function restartClient(context: ExtensionContext, reason: string): Promise<void> {
    lspOutputChannel.appendLine(reason);
    setStatus('starting', 'AngelScript: restarting the language server');

    try {
        await client?.stop();
    } catch (error) {
        lspOutputChannel.appendLine(`Failed to stop Language Client: ${error instanceof Error ? error.message : String(error)}`);
    }

    // A restart the user asked for starts the exit budget over. The stop above is an expected
    // exit, and counting it would let four deliberate restarts look like a crash loop.
    unexpectedExits = 0;
    await startClient(context);
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
