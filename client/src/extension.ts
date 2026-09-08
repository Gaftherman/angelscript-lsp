import * as path from 'path';
import * as os from 'os';
import * as fs from 'fs';
import {
    ExtensionContext, window, workspace, env, commands, OutputChannel, ExtensionMode,
    StatusBarAlignment, StatusBarItem, ThemeColor, ConfigurationTarget, QuickPickItem, Uri, l10n,
    TextEditorDecorationType, Range, TextEditor, WorkspaceEdit
} from 'vscode';
// Types only: erased at compile time, so naming them here costs nothing at runtime.
import type {
    LanguageClient, LanguageClientOptions, ServerOptions, ErrorHandler
} from 'vscode-languageclient/node';

/**
 * @brief The language client library, loaded the first time the server is actually started.
 *
 * It is nearly the whole of this extension's bundle - 364 KB, 42 ms to require - and VS Code's
 * reported activation time is that require plus activate(). activate() deliberately does not await
 * startClient, precisely so the user is not waiting on a process spawn; leaving the library on the
 * static import path put its cost back on the number they see anyway, for work nobody is waiting
 * for.
 *
 * A dynamic import moves it off that path. esbuild keeps the code in the same output file for a cjs
 * bundle, wrapped in a lazily-initialised module, so its top level runs on first call instead of at
 * load. Cached here because restarting the client must not pay for it twice.
 */
type LanguageClientModule = typeof import('vscode-languageclient/node');

let languageClientModule: LanguageClientModule | undefined;

function loadLanguageClientModule(): LanguageClientModule {
    // `require` rather than a dynamic `import()`: under "module": "Node16" TypeScript refuses to
    // resolve an `import()` of this CommonJS package from a CommonJS file. esbuild gives both the
    // same treatment in a bundle - the module becomes a lazily-initialised wrapper - so this is
    // the same deferral, synchronous, and it types cleanly.
    //
    // No eslint-disable above it: this config enables four rules and naming-convention, and
    // `@typescript-eslint/no-require-imports` is not among them - so the directive reported itself
    // as unused. Turning the recommended set on is what would need it back, and the paragraph above
    // is the reason it would be granted.
    languageClientModule ??= require('vscode-languageclient/node') as LanguageClientModule;
    return languageClientModule;
}

/**
 * @brief Whether the client exists and is up, without needing the library to be loaded.
 *
 * `State.Running` is 2. Naming the number rather than awaiting the module keeps the three callers
 * that ask this synchronous - two of them are polling loops - and a client that exists at all has
 * already loaded the library, so there is nothing to wait for either way.
 */
function clientIsRunning(): boolean {
    const running = client;
    return running !== undefined && running.state === 2;
}

/**
 * @brief When this module finished evaluating, as the baseline every activation mark is measured from.
 *
 * A user reported 2942 ms to load a small project against 500 ms on a fast machine. The server's
 * workspace scan is timed and was most of it; the client reported nothing, so whatever remained had
 * nowhere to be looked up. Everything below is measured from here.
 *
 * This cannot see the cost of loading the bundle itself - by the time a statement in it runs, that
 * is already paid. What VS Code reports as activation time includes it, so a large gap between this
 * baseline and the host's own figure is that load, and is the one number this file cannot produce.
 */
const moduleEvaluatedAt = Date.now();

/**
 * @brief Milliseconds from module evaluation to the end of each activation phase.
 *
 * Exported so a test can assert on it rather than on a log line, and so the numbers are available
 * to anyone debugging a slow start without adding instrumentation of their own.
 */
export const activationTimings: Record<string, number> = {};

/** @brief Records how long a phase took and returns its result, so a call site stays one line. */
function timed<T>(phase: string, body: () => T): T {
    const started = Date.now();
    try {
        return body();
    } finally {
        activationTimings[phase] = Date.now() - started;
    }
}

let client: LanguageClient;
let lspOutputChannel: OutputChannel;
let statusBarItem: StatusBarItem;

/** @brief Command that reveals the server log. The status bar item and every error offer it. */
const SHOW_LOG_COMMAND = 'angelscript.showServerLog';

/** @brief Command that stops the running server and starts a fresh one with current settings. */
const RESTART_COMMAND = 'angelscript.restartServer';

/** @brief Command that prompts the user to select an active predefined API stub or merge all. */
const SELECT_PREDEFINED_COMMAND = 'angelscript.selectPredefined';

/** @brief Command that opens the status bar menu: the log, a restart, and the stub picker. */
const STATUS_MENU_COMMAND = 'angelscript.statusMenu';

/** @brief Command that registers a script as a module entry point in workspace settings. */
const SET_MODULE_ENTRY_POINT_COMMAND = 'angelscript.setModuleEntryPoint';

/** @brief Command that registers a directory as a module folder in workspace settings. */
const SET_MODULE_FOLDER_COMMAND = 'angelscript.setModuleFolder';

/** @brief Command that gathers a predefined stub's scattered namespaces into one block each. */
const FORMAT_STUB_COMMAND = 'angelscript.formatPredefinedStub';

/**
 * @brief Wording of the button on every failure notification.
 *
 * Localised at the point of use rather than here: l10n.t() reads the bundle for the editor's
 * display language, and at module load there is nothing to read it against yet.
 */
const showLogAction = () => l10n.t('Show Log');

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
 * @brief Name of the predefined stub in force, shown in the status bar.
 *
 * The bar is where a user looks for this - it is where Live Share, the language mode and the line
 * ending all live - and it is the only surface that is still there after the notification about it
 * has been dismissed. Empty means the server has not said yet, or every stub is merged.
 */
let activeStubLabel = '';
let activeStubPath = '';

/**
 * @brief The line ranges the preprocessor drops, per document, as the server last reported them.
 *
 * Kept because a decoration is applied to an *editor*, and an editor for a document can appear
 * after the notification that described it - opening the file in a second group, or coming back to
 * a tab - at which point there is nothing to recompute from.
 */
const inactiveRegionsByUri = new Map<string, Range[]>();

/** @brief The dimming itself. Rebuilt when the opacity setting changes, since it bakes the value in. */
let inactiveDecoration: TextEditorDecorationType | undefined;

/**
 * @brief Dims the code inside a `#if` the preprocessor drops.
 *
 * A decoration rather than a semantic token, which is what this used to be. The editor's
 * bracket-pair colouring paints `(`, `{` and `[` from its own feature and consults neither
 * TextMate nor semantic scopes, so dead code kept rainbow brackets no matter what tokens the
 * server emitted for it. A decoration sits over everything, brackets included - and it dims the
 * syntax colours rather than replacing them, which is what the C++ extension does with its own
 * inactive regions.
 */
function ensureInactiveDecoration(): TextEditorDecorationType | undefined {
    const config = workspace.getConfiguration('angelscript');
    if (config.get<boolean>('dimInactiveRegions', true) !== true) {
        return undefined;
    }

    if (inactiveDecoration === undefined) {
        const opacity = config.get<number>('inactiveRegionOpacity', 0.55);
        inactiveDecoration = window.createTextEditorDecorationType({
            // `!important` because a theme's own rules for the scopes underneath would otherwise
            // win, and the point is to dim whatever those produced.
            opacity: `${opacity} !important`,
            isWholeLine: true
        });
    }

    return inactiveDecoration;
}

/** @brief Applies - or clears - the dimming on one editor from what the server last said. */
function applyInactiveRegions(editor: TextEditor): void {
    const decoration = ensureInactiveDecoration();
    if (decoration === undefined) {
        return;
    }

    if (editor.document.languageId !== 'angelscript') {
        return;
    }

    editor.setDecorations(decoration, inactiveRegionsByUri.get(editor.document.uri.toString()) ?? []);
}

/** @brief Throws the current decoration away, so the next apply builds one with the new opacity. */
function resetInactiveDecoration(): void {
    inactiveDecoration?.dispose();
    inactiveDecoration = undefined;
}

/**
 * @brief Restarts run one at a time, in the order they were asked for.
 *
 * Toggling three settings in the settings UI fires three configuration events in a row, and each
 * used to start its own stop-then-start against the same `client` variable. The second stop raced
 * the first start, and the loser reported "Starting server failed".
 */
let restartChain: Promise<void> = Promise.resolve();

/** @brief True while a restart has been asked for and has not begun yet. */
let restartPending = false;

/**
 * @brief The command line the running server was launched with.
 *
 * Kept so a configuration change can be told apart from one that changes nothing the server reads.
 * Comparing the arguments is exact and needs no list of "settings that matter" to keep in step with
 * package.json - a list like that goes stale the first time somebody adds a setting and forgets it,
 * and the symptom is a control that silently does nothing.
 */
let runningServerArgs: string[] = [];

/**
 * @brief Reports the server's state where the user can see it without opening a panel.
 *
 * The extension had no surface at all: a server that never started and a server working normally
 * looked identical from the editor, because the only difference was a line in an output channel
 * nobody has open. Every state here is clickable and reveals that channel.
 */
let lastStatus: { state: 'starting' | 'running' | 'failed'; tooltip: string } | undefined;

/**
 * @brief Which side of the status bar the item currently sits on.
 *
 * Tracked because VS Code fixes an item's alignment when it is created: moving it is creating a
 * new one, so the old has to be disposed and the last known state replayed onto the replacement.
 * Undefined until the first createStatusBarItem call.
 */
let statusBarAlignmentInUse: StatusBarAlignment | undefined;

/**
 * @brief Creates the status bar item on the configured side, replacing the existing one if it moved.
 *
 * Defaults to the left. The item is the way to reach the log, a restart and the stub picker, and
 * on the right it sits past whatever else the window has to say; on the left it is next to the
 * problem counts, which is where the user is already looking. `angelscript.statusBar.alignment`
 * moves it back for anyone who disagrees.
 */
function createStatusBarItem(context: ExtensionContext): void {
    const configured = workspace.getConfiguration('angelscript').get<string>('statusBar.alignment', 'left');
    const alignment = configured === 'right' ? StatusBarAlignment.Right : StatusBarAlignment.Left;

    if (statusBarItem && statusBarAlignmentInUse === alignment) {
        return;
    }

    // Disposed rather than hidden: an item left alive on the other side is a second copy of the
    // same control, and the user moved it precisely because they did not want it there.
    statusBarItem?.dispose();

    statusBarItem = window.createStatusBarItem(alignment, 100);
    statusBarItem.command = STATUS_MENU_COMMAND;
    context.subscriptions.push(statusBarItem);
    statusBarAlignmentInUse = alignment;

    // The new item starts blank, and nothing else will speak until the server changes state - which
    // it need not ever do again - so the last thing it said is replayed onto it.
    if (lastStatus) {
        setStatus(lastStatus.state, lastStatus.tooltip);
    }
}

function setStatus(state: 'starting' | 'running' | 'failed', tooltip: string): void {
    lastStatus = { state, tooltip };

    if (!statusBarItem) {
        return;
    }

    const label = { starting: '$(sync~spin) AngelScript', running: '$(check) AngelScript', failed: '$(error) AngelScript' };

    // The stub only rides along on the healthy state: on a failure the bar has something more
    // urgent to say, and while starting there is no answer yet.
    statusBarItem.text = state === 'running' && activeStubLabel.length > 0
        ? `${label[state]}: ${activeStubLabel}`
        : label[state];
    // The bar has room for a couple of path segments; the tooltip has room for the answer.
    const stubLine = state === 'running' && activeStubPath.length > 0
        ? `\n${l10n.t('Stub: {0}', activeStubPath)}`
        : '';
    statusBarItem.tooltip =
        `${tooltip}${stubLine}\n${l10n.t('Click for the log, a restart, or the stub picker.')}`;
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

    const action = showLogAction();
    void window.showErrorMessage(`AngelScript: ${summary}`, action).then(choice => {
        if (choice === action) {
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
    ['onTypeFormatting', '--disable-on-type-formatting'],
    ['pullDiagnostics', '--disable-pull-diagnostics']
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
 * @brief `angelscript.preprocessor.*` switches, in the order the server's --preprocessor-feature
 *        expects.
 *
 * Not engine properties: an engine property is an asEP_* value the SDK interprets, while these
 * describe scriptbuilder.cpp, which is a sample add-on hosts routinely copy into their own tree
 * and patch. `#else` is the first thing they add, and it does not exist in the stock file - a
 * `#else` there is swallowed by the dead block it sits in, measured against the real compiler.
 *
 * All default off, so a stock host pays nothing on the command line and sees no change at all.
 */
const PREPROCESSOR_FEATURES: ReadonlyArray<string> = [
    'elseSupport',
    'elifSupport',
    'ifdefSupport',
    'defineInScripts'
];

/**
 * @brief Expands a possibly relative path into the absolute forms to pass to the server.
 *
 * The server's working directory is not the project root, so a relative entry only means anything
 * once resolved - and in a multi-root workspace it can legitimately mean one path per folder.
 */
/**
 * @brief Expands the `${...}` variables VS Code uses in launch.json, in a setting value.
 *
 * VS Code resolves these itself in `launch.json` and `tasks.json` and nowhere else: a setting is
 * handed to the extension exactly as it was typed. So `${workspaceFolder}/host.as.predefined`
 * reached the server as a literal path with a dollar sign in it, matched no file, and the stub
 * silently did not load. The only portable spelling left was a relative path, which is why a
 * workspace that mixed one relative stub with one absolute path was the case that broke.
 *
 * Supported, and deliberately only these - each one has a single unambiguous answer:
 *   - `${workspaceFolder}`         every workspace folder, one result each (see below)
 *   - `${workspaceFolder:name}`    the folder with that name, and nothing if there is none
 *   - `${userHome}`                the user's home directory
 *   - `${env:NAME}`                an environment variable, which is how a host SDK path outside
 *                                  the workspace is usually already written down
 *
 * The bare `${workspaceFolder}` expands to one result *per folder*, the same way a relative entry
 * already did. In a single-root workspace - which is nearly all of them - that is exactly one.
 *
 * @return One entry per expansion, or none when a variable names something that does not exist.
 *         An unknown variable is left untouched rather than dropped, so it reaches the log as the
 *         path it is instead of vanishing.
 */
function expandPathVariables(entry: string): string[] {
    const folders = workspace.workspaceFolders ?? [];

    // The bare form first: it is the only one that can multiply a single entry into several, so it
    // decides how many results there are before the single-valued ones are substituted into each.
    const seeds = entry.includes('${workspaceFolder}')
        ? folders.map(folder => entry.split('${workspaceFolder}').join(folder.uri.fsPath))
        : [entry];

    const expanded: string[] = [];

    for (const seed of seeds) {
        let value = seed;
        let unresolved = false;

        value = value.replace(/\$\{workspaceFolder:([^}]+)\}/g, (whole, name: string) => {
            const folder = folders.find(candidate => candidate.name === name);
            if (!folder) {
                unresolved = true;
                return whole;
            }
            return folder.uri.fsPath;
        });

        value = value.replace(/\$\{userHome\}/g, () => os.homedir());

        value = value.replace(/\$\{env:([^}]+)\}/g, (whole, name: string) => {
            const fromEnv = process.env[name];
            if (fromEnv === undefined || fromEnv.length === 0) {
                unresolved = true;
                return whole;
            }
            return fromEnv;
        });

        if (unresolved) {
            // Optional: this runs from buildServerArgs, which is called before activate() has made
            // the channel. Without the guard an unresolvable variable crashed the whole
            // settings-to-arguments pass instead of producing one line in a log.
            lspOutputChannel?.appendLine(
                `Setting "${entry}" names something this window does not have; left as written.`);
        }

        // Normalised, so the separator the user typed does not survive into the path handed to the
        // server: `${workspaceFolder}/include` otherwise produces `E:\work/include`, which works
        // and reads like a bug every time it turns up in a log.
        expanded.push(path.normalize(value));
    }

    return expanded;
}

function resolveAgainstWorkspace(entry: string): string[] {
    const trimmed = entry.trim();
    if (trimmed.length === 0) {
        return [];
    }

    const resolved: string[] = [];

    for (const expanded of expandPathVariables(trimmed)) {
        if (path.isAbsolute(expanded)) {
            resolved.push(expanded);
            continue;
        }
        // Still relative after expansion - either it never had a variable, or it had one that
        // resolved to a relative fragment. Either way the workspace folders are what it is
        // relative to, which is the behaviour this function has always had.
        for (const folder of workspace.workspaceFolders ?? []) {
            resolved.push(path.resolve(folder.uri.fsPath, expanded));
        }
    }

    return resolved;
}

export function buildServerArgs(): string[] {
    const config = workspace.getConfiguration('angelscript');
    const args: string[] = [];

    // Directories the server must not walk. Passed first because the server treats the first
    // --exclude as replacing its built-in defaults rather than adding to them.
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

    // The one stub the workspace scan should load, when the user has chosen one. Needed on the
    // command line so a server starting fresh honours the choice; a server already running is told
    // through didChangeConfiguration instead, which is why the restart watcher ignores this flag.
    const activeStub = config.get<string>('predefined.active', '').trim();
    if (activeStub.toLowerCase() === 'all') {
        // Not a filename to resolve: it asks the server to load every stub it finds, which is what
        // it used to do whenever nothing was selected.
        args.push('--predefined-active=all');
    } else if (activeStub.length > 0) {
        for (const resolved of resolveAgainstWorkspace(activeStub)) {
            args.push(`--predefined-active=${resolved}`);
        }
    }

    // Words `#if` treats as defined, matching what the host passes to CScriptBuilder::DefineWord.
    // The server had this as a CLI flag only and nothing here emitted it, so every workspace ran
    // with an empty set - and an empty set means every `#if` block in every file is excluded, and
    // its diagnostics silently suppressed. A stub can also declare these with `#define`.
    for (const entry of config.get<string[]>('define', [])) {
        if (entry.trim().length > 0) {
            args.push(`--define=${entry.trim()}`);
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

    if (config.get<boolean>('include.implicitExtension', false) === true) {
        args.push('--implicit-include-extension=true');
    }

    for (const mod of config.get<Array<{ name?: string; entry?: string }>>('modules', [])) {
        const name = mod?.name?.trim();
        const entry = mod?.entry?.trim();
        if (!name || !entry) {
            continue;
        }
        for (const resolved of resolveAgainstWorkspace(entry)) {
            args.push(`--module=${name}=${resolved}`);
        }
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

    for (const feature of PREPROCESSOR_FEATURES) {
        if (config.get<boolean>(`preprocessor.${feature}`, false) === true) {
            args.push(`--preprocessor-feature=${feature}=true`);
        }
    }

    // Three-valued rather than a boolean, so it does not ride the loop above. Accept is the
    // default and costs nothing to leave alone.
    const pragmaMode = config.get<string>('preprocessor.pragmaMode', 'accept');
    if (pragmaMode !== 'accept') {
        args.push(`--preprocessor-feature=pragmaMode=${pragmaMode}`);
    }

    // Opt-in diagnostics. Not a feature switch and not an engine option: a rule that is right for
    // a workspace whose declarations are complete and wrong for one whose host registers types in
    // C++. Passed on only when asked for, so an untouched setting stays off the command line.
    if (config.get<boolean>('diagnostics.reportUnknownTypes', true) === false) {
        args.push('--no-report-unknown-types');
    }
    if (config.get<boolean>('diagnostics.reportAccessorPortability', false) === true) {
        args.push('--report-accessor-portability');
    }
    if (config.get<boolean>('diagnostics.reportAccessorDisabled', false) === true) {
        args.push('--report-accessor-disabled');
    }
    if (config.get<boolean>('diagnostics.reportBoolConversion', false) === true) {
        args.push('--report-bool-conversion');
    }
    if (config.get<boolean>('diagnostics.reportMissingFuncdef', false) === true) {
        args.push('--report-missing-funcdef');
    }
    if (config.get<boolean>('diagnostics.reportIntegerDivision', false) === true) {
        args.push('--report-integer-division');
    }

    // asEP_PROPERTY_ACCESSOR_MODE takes a number, not a boolean, so it is not one of
    // ENGINE_PROPERTIES above either. The test against 2 and 3 is a whitelist, not a
    // default-skipping check: those are the only values package.json offers, and a number arriving
    // from a hand-edited settings.json is dropped rather than passed through to be rejected by the
    // server's own parser.
    const accessorMode = config.get<number>('engine.propertyAccessorMode', 2);
    if (accessorMode === 2 || accessorMode === 3) {
        args.push(`--engine-property=propertyAccessorMode=${accessorMode}`);
    }
    // A boolean engine property, so it rides the same --engine-property channel as the numeric
    // ones rather than getting a flag of its own.
    if (config.get<boolean>('engine.allowMultilineStrings', false) === true) {
        args.push('--engine-property=allowMultilineStrings=1');
    }

    const boolConversionMode = config.get<number>('engine.boolConversionMode', 0);
    if (boolConversionMode === 0 || boolConversionMode === 1) {
        args.push(`--engine-property=boolConversionMode=${boolConversionMode}`);
    }

    const useCharacterLiterals = config.get<number>('engine.useCharacterLiterals', 0);
    if (useCharacterLiterals === 0 || useCharacterLiterals === 1) {
        args.push(`--engine-property=useCharacterLiterals=${useCharacterLiterals}`);
    }

    if (config.get<boolean>('engine.disallowValueAssignForRef', false) === true) {
        args.push('--engine-property=disallowValueAssignForRef=1');
    }

    const alterSyntaxNamedArgs = config.get<number>('engine.alterSyntaxNamedArgs', 0);
    if (alterSyntaxNamedArgs === 0 || alterSyntaxNamedArgs === 1 || alterSyntaxNamedArgs === 2) {
        args.push(`--engine-property=alterSyntaxNamedArgs=${alterSyntaxNamedArgs}`);
    }

    if (config.get<boolean>('engine.disableIntegerDivision', false) === true) {
        args.push('--engine-property=disableIntegerDivision=1');
    }

    if (config.get<boolean>('engine.disallowEmptyListElements', false) === true) {
        args.push('--engine-property=disallowEmptyListElements=1');
    }

    if (config.get<boolean>('engine.foreachSupport', true) === false) {
        args.push('--engine-property=foreachSupport=0');
    }

    // The five measured on 2026-09-05. Twelve engine properties were unmodelled; probing each
    // against the real compiler showed only these five change a verdict a reader of source could
    // see, and the other seven were left alone rather than modelled on faith - the same answer
    // asEP_HEREDOC_TRIM_MODE got.
    if (config.get<boolean>('engine.requireEnumScope', false) === true) {
        args.push('--engine-property=requireEnumScope=1');
    }

    // Measured, not assumed, and the assumption was wrong first time round: running the oracle with
    // no flag gives the same verdict as running it with this property set to 0, so the engine's own
    // default is OFF. Turning it ON is what makes `class C { C(int a) {} } C c;` start compiling.
    if (config.get<boolean>('engine.alwaysImplDefaultConstruct', false) === true) {
        args.push('--engine-property=alwaysImplDefaultConstruct=1');
    }

    if (config.get<boolean>('engine.allowUnicodeIdentifiers', false) === true) {
        args.push('--engine-property=allowUnicodeIdentifiers=1');
    }

    if (config.get<boolean>('engine.ignoreDuplicateSharedIntf', false) === true) {
        args.push('--engine-property=ignoreDuplicateSharedIntf=1');
    }

    // A number, not a boolean, and the only one that moves a severity rather than deciding whether
    // something is reported at all: 0 suppresses every warning, 1 is the engine's default, 2 turns
    // warnings into errors.
    const compilerWarnings = config.get<number>('engine.compilerWarnings', 1);
    if (compilerWarnings !== 1) {
        args.push(`--engine-property=compilerWarnings=${compilerWarnings}`);
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
    if (config.get<boolean>('format.onSave', false) === true) {
        args.push('--format-on-save');
    }

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
    const server = timed('resolveServerBinary', () => resolveServerBinary(context));

    lspOutputChannel.appendLine("--- AngelScript C++ Language Server Activation ---");
    lspOutputChannel.appendLine(`Runtime Platform Context: ${os.platform()}-${os.arch()}`);
    lspOutputChannel.appendLine(`Resolved Server Binary Path: ${server.path}`);

    // Checked before launching rather than left to the spawn. A missing binary is the one failure
    // with a specific, actionable cause - the extension carries no build for this platform, or a
    // source checkout has not been built - and letting it arrive as a bare ENOENT threw that away.
    if (!server.found) {
        reportFailure(
            l10n.t('no language server binary for {0}. Editor features are unavailable.',
                   `${os.platform()}-${os.arch()}`),
            `Looked in:\n  ${server.searched.join('\n  ')}`);
        return;
    }

    setStatus('starting', l10n.t('Starting the AngelScript language server.'));

    // The first thing that needs the library, and the point its cost belongs at: the user is not
    // waiting on this call - activate() did not await it.
    const startedLoadingAt = Date.now();
    const { LanguageClient, ErrorAction, CloseAction } = loadLanguageClientModule();
    activationTimings['loadLanguageClientModule'] = Date.now() - startedLoadingAt;

    const serverArgs = timed('buildServerArgs', () => buildServerArgs());
    runningServerArgs = serverArgs;
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
                    l10n.t('the language server connection keeps failing, so it has been shut down.'),
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
                    l10n.t('the language server stopped {0} times and will not be restarted again.',
                           unexpectedExits));
                return { action: CloseAction.DoNotRestart, handled: true };
            }
            lspOutputChannel.appendLine(
                `Language server exited unexpectedly (${unexpectedExits}/${MAX_SILENT_RESTARTS}); restarting.`);
            return { action: CloseAction.Restart, handled: true };
        }
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'angelscript' }],
        // No `configurationSection` here on purpose. It installs a second configuration listener
        // inside the LanguageClient, and that one fired against a client this extension was already
        // restarting for the same event - which surfaced as "Sending notification
        // workspace/didChangeConfiguration failed / Error: Starting server failed" on every toggle
        // in the settings UI. There is one listener now, below, and it decides between telling the
        // running server and replacing it.
        synchronize: {
            // Without this the server only ever learns about files the user opened: a branch
            // switch, a pull, or a generated script would leave every stale symbol in the index.
            // angelscript.exclude does NOT apply here: createFileSystemWatcher takes a single
            // include glob and no exclusions, so a build tree full of .as files is still watched.
            // VS Code's own `files.watcherExclude` governs that, and it already excludes .git and
            // node_modules by default. Said plainly because the server-side scans ARE pruned, and
            // assuming the watcher followed them would be the natural mistake to make.
            fileEvents: timed('createFileSystemWatcher',
                              () => workspace.createFileSystemWatcher('**/*.{as,angelscript,predefined}'))
        },
        outputChannel: lspOutputChannel,
        errorHandler
    };

    client = timed('constructLanguageClient', () => new LanguageClient(
        'angelScriptLSP',
        'AngelScript C++ Language Server',
        serverOptions,
        clientOptions
    ));

    try {
        const startedAt = Date.now();
        await client.start();
        activationTimings['clientStart'] = Date.now() - startedAt;

        // One line, because a user asked to attach one. `clientStart` is the process spawn plus the
        // initialize handshake, and on a slow machine it is expected to dominate the rest.
        lspOutputChannel.appendLine(
            'Activation timings (ms): ' +
            Object.entries(activationTimings).map(([phase, ms]) => `${phase}=${ms}`).join(' '));

        unexpectedExits = 0;
        setStatus('running', l10n.t('The AngelScript language server is running.'));

        // Not awaited: it waits out a workspace scan, and nothing else here depends on it.
        void offerStubChoice();
        client.onNotification("angelscript/debug", (params: { message: string }) => {
            lspOutputChannel.appendLine(`[AST Debug] ${params.message}`);
        });

        client.onNotification("angelscript/inactiveRegions",
            (params: { uri: string; regions: { startLine: number; endLine: number }[] }) => {
                // Stored under the document's own key rather than the server's spelling of the URI,
                // so looking it up from an editor cannot miss on a percent-encoded drive letter.
                const key = Uri.parse(params.uri).toString();
                inactiveRegionsByUri.set(
                    key,
                    (params.regions ?? []).map(region =>
                        new Range(region.startLine, 0, region.endLine, Number.MAX_SAFE_INTEGER)));

                for (const editor of window.visibleTextEditors) {
                    if (editor.document.uri.toString() === key) {
                        applyInactiveRegions(editor);
                    }
                }
            });
    } catch (error) {
        reportFailure(
            l10n.t('the language server failed to start. Editor features are unavailable.'),
            `Failed to start Language Client: ${error instanceof Error ? error.stack ?? error.message : String(error)}`);
    }
}


/**
 * @brief Activates the AngelScript Language Client extension interface handlers.
 * @param context The extension context provided by VS Code.
 */
export async function activate(context: ExtensionContext) {
    const activateEnteredAt = Date.now();
    activationTimings['moduleToActivate'] = activateEnteredAt - moduleEvaluatedAt;

    lspOutputChannel = timed('outputChannel', () => {
        const channel = window.createOutputChannel('AngelScript C++ Language Server');
        context.subscriptions.push(channel);
        return channel;
    });

    timed('registerCommands', () => {
        context.subscriptions.push(
            commands.registerCommand(SHOW_LOG_COMMAND, () => lspOutputChannel.show(true)));

        context.subscriptions.push(
            commands.registerCommand(RESTART_COMMAND, () => restartClient(context, 'Restart requested from the command palette.')));

        context.subscriptions.push(
            commands.registerCommand(SELECT_PREDEFINED_COMMAND, () => selectPredefinedStub()));

        context.subscriptions.push(
            commands.registerCommand(STATUS_MENU_COMMAND, () => showStatusMenu(context)));

        context.subscriptions.push(
            commands.registerCommand(SET_MODULE_ENTRY_POINT_COMMAND, (resource?: Uri) => setModuleEntryPoint(resource)));

        context.subscriptions.push(
            commands.registerCommand(SET_MODULE_FOLDER_COMMAND, (resource?: Uri) => setModuleFolder(resource)));

        context.subscriptions.push(
            commands.registerCommand(FORMAT_STUB_COMMAND, (resource?: Uri) => formatPredefinedStub(resource)));
    });

    // An editor can appear after the notification that described its document - a second group, or
    // a tab returned to - and there is nothing to recompute from at that point, so the last thing
    // the server said is replayed onto it.
    context.subscriptions.push(
        window.onDidChangeVisibleTextEditors(editors => editors.forEach(applyInactiveRegions)));

    timed('statusBarItem', () => createStatusBarItem(context));

    // Deliberately not awaited. Everything this extension contributes to the UI - the commands,
    // the status bar item, the output channel - is registered above and ready now; what follows is
    // spawning a process and waiting out a protocol handshake, and holding activate() open for it
    // put this extension near 1.6s in VS Code's activation report for work the user was not waiting
    // on. Failures still reach them: startClient reports every one itself, through reportFailure.
    //
    // Safe to leave running because the parts activate() still needs from it are set before its
    // first await: resolveServerBinary and buildServerArgs are synchronous, so runningServerArgs
    // holds the real command line by the time the listener below can read it.
    restartChain = startClient(context).catch(error => {
        // startClient reports its own failures; this catches the ones it cannot, so an unexpected
        // throw becomes a line the user can act on rather than an unhandled rejection in a log
        // they will never open.
        reportFailure(
            l10n.t('the language server failed to start. Editor features are unavailable.'),
            error instanceof Error ? error.stack ?? error.message : String(error));
    });

    // Settings that become command-line arguments cannot take effect in a running server, so the
    // client is rebuilt instead. Without this the user has to reload the whole window for a
    // setting to do anything, which reads as the setting being broken.
    context.subscriptions.push(
        workspace.onDidChangeConfiguration(async event => {
            if (!event.affectsConfiguration('angelscript')) {
                return;
            }

            // A setting only needs a restart if it changes what the server was launched with.
            // `predefined.active` is the exception that proves it: it *is* on the command line, so
            // a fresh server gets it, but the running one is told through didChangeConfiguration
            // and reloads on its own. Restarting for it would tear down and redo the whole
            // workspace scan to reach the state the server had already reached.
            const withoutActiveStub = (args: string[]) =>
                args.filter(arg => !arg.startsWith('--predefined-active='));

            // The decoration bakes the opacity in, so a change to either setting has to build a
            // new one. Cheap, and it happens only when the user edits the setting.
            if (event.affectsConfiguration('angelscript.inactiveRegionOpacity') ||
                event.affectsConfiguration('angelscript.dimInactiveRegions')) {
                resetInactiveDecoration();
                window.visibleTextEditors.forEach(applyInactiveRegions);
            }

            // Purely a client-side control, so it moves without touching the server. Handled before
            // the command-line comparison below, which would otherwise decide nothing changed and
            // leave the item where it was.
            if (event.affectsConfiguration('angelscript.statusBar.alignment')) {
                createStatusBarItem(context);
            }

            const next = buildServerArgs();
            const unchanged =
                withoutActiveStub(next).join('\u0000') === withoutActiveStub(runningServerArgs).join('\u0000');

            runningServerArgs = next;

            if (unchanged) {
                // Nothing on the command line moved, so the running server can be told rather than
                // replaced. This is the path every hot-reloadable setting takes: the active stub,
                // the engine profile, the search directories, the brace style.
                await pushConfiguration();
                return;
            }

            await restartClient(context, 'Configuration changed; restarting the language server.');
        })
    );

    // Handed back through the extension's exports, which is the only way a test can read them:
    // VS Code loads the bundle at dist/extension.js while the test harness imports
    // out/extension.js, so a test importing this module gets its own empty copy of the map.
    // The same object is returned rather than a copy, so `clientStart` - recorded after activate
    // has already returned - appears in it too.
    return { activationTimings };
}

/**
 * @brief The menu behind the status bar item.
 *
 * The item used to run one command - reveal the log - and everything else this extension does was
 * reachable only by knowing its name in the command palette. Restarting the server and choosing a
 * stub are the two things a user actually needs when something looks wrong, so they are one click
 * from the thing that told them something was wrong.
 */
async function showStatusMenu(context: ExtensionContext): Promise<void> {
    interface ActionItem extends QuickPickItem {
        run: () => void | Promise<void>;
    }

    const actions: ActionItem[] = [
        {
            label: '$(output) ' + l10n.t('Show Server Log'),
            description: l10n.t('Everything the server has reported this session.'),
            run: () => lspOutputChannel.show(true)
        },
        {
            label: '$(debug-restart) ' + l10n.t('Restart Server'),
            description: l10n.t('Stops the language server and starts a fresh one.'),
            run: () => restartClient(context, 'Restart requested from the status bar.')
        },
        {
            label: '$(library) ' + l10n.t('Select Predefined Stub'),
            description: l10n.t('Chooses which host API description the workspace uses.'),
            run: () => selectPredefinedStub()
        }
    ];

    const chosen = await window.showQuickPick(actions, {
        placeHolder: l10n.t('AngelScript language server')
    });

    await chosen?.run();
}

/**
 * @brief Hands the running server the current `angelscript` settings section.
 *
 * Sent only while the client is actually running: a notification to one that is starting or
 * stopping makes the language client try to start it, which is the failure this replaced.
 */
/**
 * @brief Expands the path variables in the settings object handed to a running server.
 *
 * The command line goes through resolveAgainstWorkspace; this is the other half. Without it a
 * setting changed while the server is running arrived with `${workspaceFolder}` still in it, so the
 * same value worked at startup and stopped working the moment it was edited - which is the worst
 * version of this bug, because it looks like the edit was the problem.
 *
 * Only the four path-valued settings are touched. Everything else is copied through untouched: a
 * `$` in a define or an exclude glob is not a path variable, and rewriting it would be a surprise.
 */
function expandConfiguredPaths(settings: unknown): unknown {
    if (typeof settings !== 'object' || settings === null) {
        return settings;
    }

    const copy: Record<string, unknown> = { ...(settings as Record<string, unknown>) };

    const expandList = (key: string) => {
        const value = copy[key];
        if (Array.isArray(value)) {
            copy[key] = value.flatMap(entry =>
                typeof entry === 'string' ? resolveAgainstWorkspace(entry) : [entry]);
        }
    };

    expandList('searchDirectories');
    expandList('predefinedFiles');

    const expandOne = (key: string) => {
        const value = copy[key];
        if (typeof value === 'string' && value.trim().length > 0 && value.trim().toLowerCase() !== 'all') {
            // One setting, one value: in a multi-root workspace a bare `${workspaceFolder}` has
            // several answers and this field can hold one, so the first folder wins. The command
            // line has the same shape and the same limit.
            const [first] = resolveAgainstWorkspace(value);
            if (first !== undefined) {
                copy[key] = first;
            }
        }
    };

    expandOne('predefinedFile');

    // `modules` carries its paths one level down, in `entry` and `folder`, and was never expanded
    // here. A module set from the Explorer context menu therefore did nothing until the next
    // restart: the setting is written as `${workspaceFolder}/...` so it survives a different
    // checkout, the command line expands it, and this notification handed the server the literal
    // string to resolve against its own working directory.
    const modules = copy['modules'];
    if (Array.isArray(modules)) {
        copy['modules'] = modules.map(item => {
            if (typeof item !== 'object' || item === null) {
                return item;
            }

            const module: Record<string, unknown> = { ...(item as Record<string, unknown>) };
            for (const key of ['entry', 'folder']) {
                const value = module[key];
                if (typeof value === 'string' && value.trim().length > 0) {
                    // The first expansion, as expandOne explains: one field holds one path, and a
                    // bare `${workspaceFolder}` in a multi-root window has several answers.
                    const [first] = resolveAgainstWorkspace(value);
                    if (first !== undefined) {
                        module[key] = first;
                    }
                }
            }
            return module;
        });
    }

    const predefined = copy['predefined'];
    if (typeof predefined === 'object' && predefined !== null) {
        const nested: Record<string, unknown> = { ...(predefined as Record<string, unknown>) };
        const active = nested['active'];
        if (typeof active === 'string' && active.trim().length > 0 && active.trim().toLowerCase() !== 'all') {
            const [first] = resolveAgainstWorkspace(active);
            if (first !== undefined) {
                nested['active'] = first;
            }
        }
        copy['predefined'] = nested;
    }

    return copy;
}

/**
 * @brief Rewrites a chosen stub path as `${workspaceFolder}/...` when it lives inside one.
 *
 * The picker used to write the absolute path it had in hand, which pins the setting to one
 * machine: committed to a repository it names a drive letter and a user directory that nobody else
 * has. A stub outside every folder keeps its absolute path, because there is nothing else it could
 * be - and the two can now sit in the same setting without either being wrong, which is the case
 * that used to break.
 *
 * `all` is a request rather than a path and passes through untouched.
 */
export function portableStubPath(stubPath: string): string {
    if (stubPath.toLowerCase() === 'all' || !path.isAbsolute(stubPath)) {
        return stubPath;
    }

    for (const folder of workspace.workspaceFolders ?? []) {
        const relative = path.relative(folder.uri.fsPath, stubPath);

        // Inside the folder, rather than merely near it: a sibling directory produces a relative
        // path that starts with `..`, and `${workspaceFolder}/../other` is portable in name only.
        if (relative.length > 0 && !relative.startsWith('..') && !path.isAbsolute(relative)) {
            // Forward slashes on every platform. Windows accepts them, and a setting written with
            // backslashes has to be escaped in JSON, which is how these get mistyped.
            return '${workspaceFolder}/' + relative.split(path.sep).join('/');
        }
    }

    return stubPath;
}

async function pushConfiguration(): Promise<void> {
    const running = client;
    if (!running) {
        return;
    }

    // Awaited rather than imported at the top for the reason in loadLanguageClientModule. There is
    // always a client by the time this runs, so the module is already loaded and this resolves
    // immediately.
    if (!clientIsRunning()) {
        return;
    }

    const { DidChangeConfigurationNotification } = loadLanguageClientModule();

    try {
        await running.sendNotification(DidChangeConfigurationNotification.type, {
            settings: { angelscript: expandConfiguredPaths(workspace.getConfiguration().get('angelscript') ?? {}) }
        });
    } catch (error) {
        lspOutputChannel.appendLine(
            `Could not hand the server its new configuration: ${error instanceof Error ? error.message : String(error)}`);
    }
}

/**
 * @brief Queues a restart behind any restart already running or waiting.
 *
 * A request made while one is already waiting is folded into it rather than queued: every restart
 * reads the configuration as it starts, so the one already waiting will carry this change too.
 *
 * @param context The extension execution context.
 * @param reason Written to the log, so a restart is never a silent gap in it.
 */
function restartClient(context: ExtensionContext, reason: string): Promise<void> {
    if (restartPending) {
        lspOutputChannel.appendLine(`${reason} (folded into the restart already queued)`);
        return restartChain;
    }

    restartPending = true;
    restartChain = restartChain.catch(() => { /* a failed restart must not block the next one */ })
        .then(async () => {
            restartPending = false;
            await performRestart(context, reason);
        });

    return restartChain;
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
async function performRestart(context: ExtensionContext, reason: string): Promise<void> {
    lspOutputChannel.appendLine(reason);
    setStatus('starting', l10n.t('Restarting the AngelScript language server.'));

    if (client) {
        const oldClient = client;
        client = undefined as unknown as LanguageClient;
        try {
            await oldClient.stop();
            await oldClient.dispose();
        } catch (error) {
            lspOutputChannel.appendLine(
                `Failed to stop Language Client: ${error instanceof Error ? error.message : String(error)}`);
        }
    }

    // A restart the user asked for starts the exit budget over. The stop above is an expected
    // exit, and counting it would let four deliberate restarts look like a crash loop.
    unexpectedExits = 0;
    await startClient(context);
}

/** @brief What `angelscript.listPredefinedStubs` answers. */
interface PredefinedStubsResult {
    stubs: string[];
    /** The stub actually loaded - chosen, or picked by the scan. Empty while merging. */
    active: string;
    /** True when every discovered stub is being loaded together. */
    merging?: boolean;
}

/**
 * @brief Reads the stub in force from the server and puts its name in the status bar.
 *
 * @param stubs Optionally the answer already in hand, so the caller that just asked does not ask
 *        twice.
 */
/**
 * @brief Names a stub in the fewest path segments that can still tell it from its neighbours.
 *
 * The basename alone was the obvious choice and the wrong one: the file is called `as.predefined`
 * in almost every workspace, so with two of them the bar read `AngelScript: as.predefined` for
 * either, which is exactly the ambiguity the picker exists to resolve.
 *
 * Relative to the workspace folder when it is inside one, and short enough to sit in a status bar:
 * the last two segments, so `scripts/as.predefined` rather than the whole absolute path. A name
 * that is already distinctive - `svencoop.as.predefined` - keeps its own basename and gains
 * nothing from the folder. The full path is in the tooltip, where there is room for it.
 */
function describeStubPath(fullPath: string): string {
    const normalised = fullPath.replace(/\\/g, '/');
    const base = path.basename(normalised);

    const folder = workspace.workspaceFolders?.find(
        candidate => normalised.toLowerCase().startsWith(
            candidate.uri.fsPath.replace(/\\/g, '/').toLowerCase() + '/'));

    const relative = folder
        ? normalised.slice(folder.uri.fsPath.length + 1)
        : normalised;

    // A basename that already says which stub this is needs no folder in front of it.
    if (base !== 'as.predefined') {
        return base;
    }

    const segments = relative.split('/').filter(segment => segment.length > 0);
    return segments.length > 1 ? segments.slice(-2).join('/') : base;
}

async function refreshStubStatus(stubs?: PredefinedStubsResult): Promise<void> {
    let result = stubs;

    if (result === undefined) {
        if (!clientIsRunning()) {
            return;
        }
        try {
            result = await client.sendRequest<PredefinedStubsResult>(
                'workspace/executeCommand', { command: 'angelscript.listPredefinedStubs' });
        } catch {
            return;
        }
    }

    if (result?.merging === true) {
        activeStubLabel = l10n.t('all stubs');
        activeStubPath = '';
    } else if (result && typeof result.active === 'string' && result.active.length > 0) {
        activeStubPath = result.active;
        activeStubLabel = describeStubPath(result.active);
    } else {
        activeStubLabel = '';
        activeStubPath = '';
    }

    if (lastStatus) {
        setStatus(lastStatus.state, lastStatus.tooltip);
    }
}

/**
 * @brief Offers the picker when the workspace holds more than one stub and none was chosen.
 *
 * The server used to raise this itself, as a notification naming a command for the user to go and
 * find in the palette - which is the version that did not work. A message whose whole purpose is to
 * offer a choice has to be able to offer it, and only a client with a picker can.
 */
async function offerStubChoice(): Promise<void> {
    if (workspace.getConfiguration('angelscript').get<string>('predefined.active', '').trim().length > 0) {
        // Already answered - asking again on every server start would be nagging. The bar still has
        // to catch up, and it is the way back to the picker once this notification is gone.
        void refreshStubStatus();
        return;
    }

    // The list exists only once the workspace scan has produced it, and there is nothing in the
    // protocol a client can wait on for that. A few tries a second apart rather than one guessed
    // delay: it costs a single request when the answer is ready immediately, and it gives up rather
    // than polling a workspace that simply has no stubs in it.
    for (let attempt = 0; attempt < 10; attempt++) {
        if (!clientIsRunning()) {
            return;
        }

        let result: PredefinedStubsResult | undefined;
        try {
            result = await client.sendRequest<PredefinedStubsResult>(
                'workspace/executeCommand', { command: 'angelscript.listPredefinedStubs' });
        } catch {
            return;
        }

        if (result && Array.isArray(result.stubs) && result.stubs.length > 0) {
            // Whatever else happens, the bar can name it now.
            await refreshStubStatus(result);

            if (result.stubs.length < 2) {
                // Nothing to choose between, but the bar still says which one it is.
                return;
            }

            const action = l10n.t('Select stub');
            const picked = await window.showInformationMessage(
                l10n.t('AngelScript found {0} predefined stubs and is using {1}.',
                       result.stubs.length, path.basename(result.active ?? '')),
                action);

            if (picked === action) {
                await selectPredefinedStub();
            }
            return;
        }

        await new Promise(resolve => setTimeout(resolve, 1000));
    }
}

/**
 * @brief Prompts the user to pick an active predefined API stub or merge all discovered stubs.
 *
 * The server dynamically hot-reloads the chosen stub without requiring a client restart.
 * Querying the server for discovered stubs keeps the extension agnostic to workspace scan internals.
 */
async function selectPredefinedStub(): Promise<void> {
    if (!client) {
        void window.showWarningMessage(l10n.t('The AngelScript language server is not running.'));
        return;
    }

    let result: PredefinedStubsResult;
    try {
        result = await client.sendRequest<PredefinedStubsResult>(
            'workspace/executeCommand',
            { command: 'angelscript.listPredefinedStubs' }
        );
    } catch {
        void window.showWarningMessage(l10n.t('The AngelScript language server is not running.'));
        return;
    }

    if (!result || !Array.isArray(result.stubs)) {
        return;
    }

    interface StubQuickPickItem extends QuickPickItem {
        stubPath: string;
    }

    const items: StubQuickPickItem[] = [
        {
            label: result.merging === true || (result.active ?? '').length > 0
                ? l10n.t('Automatic')
                : `$(check) ${l10n.t('Automatic')}`,
            description: l10n.t('Default. The workspace scan loads the first stub it finds, in path order.'),
            stubPath: ''
        }
    ];

    const activeNorm = path.normalize(result.active ?? '');

    for (const stubPath of result.stubs) {
        const fileName = path.basename(stubPath);
        const isActive = activeNorm.length > 0 &&
            (stubPath === result.active || path.normalize(stubPath).toLowerCase() === activeNorm.toLowerCase());
        const label = isActive ? `$(check) ${fileName}` : fileName;

        let description = stubPath;
        const folder = workspace.getWorkspaceFolder(Uri.file(stubPath));
        if (folder) {
            description = path.relative(folder.uri.fsPath, stubPath);
        }

        items.push({
            label,
            description,
            stubPath
        });
    }

    items.push({
        label: result.merging === true
            ? `$(check) ${l10n.t('All stubs (merged)')}`
            : l10n.t('All stubs (merged)'),
        description: l10n.t('Loads every discovered stub. Declarations they share will resolve more than once.'),
        stubPath: 'all'
    });

    const chosen = await window.showQuickPick(items, {
        placeHolder: l10n.t('Select the predefined stub that describes the host application API')
    });

    if (!chosen) {
        return;
    }

    await workspace.getConfiguration('angelscript')
        .update('predefined.active', portableStubPath(chosen.stubPath), ConfigurationTarget.Workspace);

    // The server reloads on its own, and the bar has to follow. Not awaited on the rescan, because
    // there is nothing to wait on: this reads what the server has now and will be right on the next
    // read if the scan is still running.
    void refreshStubStatus();
}

/** @brief What `angelscript.formatPredefinedStub` answers. */
interface FormatStubResult {
    changed?: boolean;
    text?: string;
}

/**
 * @brief Formats a predefined stub, gathering each namespace it scatters into one block.
 *
 * A stub generated from an engine's registration table writes one declaration per registered
 * entity, so a namespace with twenty members arrives as twenty namespaces. The server owns the
 * transformation - it is the side that parses AngelScript - and answers with the text, which is
 * applied here rather than pushed as a server edit: the user pressed a button, so the change
 * belongs in their editor's undo stack.
 *
 * @param resource The stub clicked in the explorer, or undefined when run from the editor.
 */
async function formatPredefinedStub(resource?: Uri): Promise<void> {
    const target = resource ?? window.activeTextEditor?.document.uri;
    if (!target) {
        void window.showInformationMessage(
            l10n.t('Open a predefined stub, or right-click one in the explorer, to format it.'));
        return;
    }

    if (!clientIsRunning()) {
        void window.showWarningMessage(
            l10n.t('The AngelScript server is not running, so the stub cannot be formatted.'));
        return;
    }

    let result: FormatStubResult | undefined;
    try {
        result = await client.sendRequest<FormatStubResult>('workspace/executeCommand', {
            command: FORMAT_STUB_COMMAND,
            arguments: [target.toString()]
        });
    } catch (error) {
        void window.showErrorMessage(l10n.t(
            'Could not format the stub: {0}',
            error instanceof Error ? error.message : String(error)));
        return;
    }

    if (!result || typeof result.text !== 'string') {
        void window.showErrorMessage(
            l10n.t('The server did not return formatted text for this stub.'));
        return;
    }

    if (result.changed !== true) {
        void window.showInformationMessage(l10n.t('This stub is already formatted.'));
        return;
    }

    // Opened first so the rewrite lands in a buffer the user can read and undo, rather than on
    // disk under a file they cannot see.
    const document = await workspace.openTextDocument(target);
    const wholeDocument = new Range(
        document.positionAt(0), document.positionAt(document.getText().length));

    const edit = new WorkspaceEdit();
    edit.replace(document.uri, wholeDocument, result.text);

    if (await workspace.applyEdit(edit)) {
        await window.showTextDocument(document);
        void window.showInformationMessage(
            l10n.t('Formatted "{0}". Save it to keep the change.', path.basename(target.fsPath)));
    } else {
        void window.showErrorMessage(l10n.t('The edit that formats this stub was rejected.'));
    }
}

/** @brief An entry in the `angelscript.modules` workspace setting. */
interface ModuleConfig {
    name: string;
    folder?: string;
    entry?: string;
}

/**
 * @brief Prompts for a module name and associates the clicked resource with it in workspace settings.
 *
 * Persisting module membership in `.vscode/settings.json` makes module boundaries portable across
 * checkouts. When a module with the given name already exists, updating `entry` or `folder` in place
 * allows compound `{name, folder, entry}` declarations to be configured incrementally from the
 * Explorer without editing JSON by hand.
 *
 * @param resource The file or directory clicked in the Explorer context menu.
 * @param kind Whether the resource is being set as an entry point script or a module folder.
 */
async function configureModule(resource: Uri, kind: 'entry' | 'folder'): Promise<void> {
    const defaultName = kind === 'entry'
        ? path.basename(resource.fsPath, path.extname(resource.fsPath))
        : path.basename(resource.fsPath);

    const prompt = kind === 'entry'
        ? l10n.t('Enter the module name for this entry point')
        : l10n.t('Enter the module name for this folder');

    const name = await window.showInputBox({
        value: defaultName,
        prompt,
        placeHolder: l10n.t('Module name'),
        validateInput: value => {
            return value.trim().length === 0
                ? l10n.t('Module name cannot be empty.')
                : undefined;
        }
    });

    if (name === undefined) {
        return;
    }

    const trimmedName = name.trim();
    if (trimmedName.length === 0) {
        return;
    }

    const config = workspace.getConfiguration('angelscript');
    const current = config.get<ModuleConfig[]>('modules', []);
    const next = current.map(item => ({ ...item }));

    const portablePath = portableStubPath(resource.fsPath);
    const existing = next.find(item => item.name === trimmedName);

    if (existing) {
        if (kind === 'entry') {
            existing.entry = portablePath;
        } else {
            existing.folder = portablePath;
        }
    } else {
        next.push(
            kind === 'entry'
                ? { name: trimmedName, entry: portablePath }
                : { name: trimmedName, folder: portablePath }
        );
    }

    await workspace.getConfiguration('angelscript').update('modules', next, ConfigurationTarget.Workspace);

    void window.showInformationMessage(
        kind === 'entry'
            ? l10n.t('Set "{0}" as entry point for module "{1}".', portablePath, trimmedName)
            : l10n.t('Set "{0}" as folder for module "{1}".', portablePath, trimmedName)
    );
}

/**
 * @brief Registers the selected AngelScript file as a compilation entry point for a named module.
 *
 * Hosts compile one entry script which pulls in dependencies via `#include`. Marking a file as an
 * entry point tells the language server where to begin whole-module diagnostic passes and symbol
 * resolution for that compilation unit, without requiring the user to construct JSON objects by
 * hand in settings.json.
 *
 * @param resource The file URI clicked in the Explorer context menu.
 */
export async function setModuleEntryPoint(resource?: Uri): Promise<void> {
    if (!resource) {
        void window.showInformationMessage(
            l10n.t('This command is used from the explorer context menu.'));
        return;
    }

    await configureModule(resource, 'entry');
}

/**
 * @brief Registers the selected directory as a module folder in workspace settings.
 *
 * Game hosts like Sven Co-op partition scripts by folder (`scripts/maps`, `scripts/plugins`) where
 * directory membership defines the module rather than an explicit entry file. Setting a folder
 * claims every script under it without requiring `#include` graph tracking.
 *
 * @param resource The folder URI clicked in the Explorer context menu.
 */
export async function setModuleFolder(resource?: Uri): Promise<void> {
    if (!resource) {
        void window.showInformationMessage(
            l10n.t('This command is used from the explorer context menu.'));
        return;
    }

    await configureModule(resource, 'folder');
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
