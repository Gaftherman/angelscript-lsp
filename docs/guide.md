# Angelscript Detailed Guide

**[English](guide.md)** | **[Español](guide.es.md)** | **[README](../README.md)**

This guide covers workspace configuration, predefined API files, engine rules, engine dialect options, commands, and every extension setting.

## Workspace Setup Examples

Create or edit `.vscode/settings.json` inside `${workspaceFolder}` to match how your game organizes scripts.

When `angelscript.modules` is set, the editor groups scripts into separate build units called modules. That stops unrelated scripts in the same folder from colliding or hiding errors in `shared` types.

### Example A: Sven Co-op (Folder Module)

Use this when every script inside a map folder belongs to one module.

```jsonc
{
  // Allow extensionless includes (for example, #include "helper" finds helper.as)
  "angelscript.include.implicitExtension": true,

  // Extra search folder for #include paths
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],

  // All scripts under maps/hcas belong to module HCAS
  "angelscript.modules": [
    {
      "name": "HCAS",
      "folder": "${workspaceFolder}/maps/hcas"
    }
  ],

  // Active predefined game API file
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

### Example B: Sven Co-op (Entry Script Module)

Use this when your game compiles a script tree starting from a single registration file.

```jsonc
{
  "angelscript.include.implicitExtension": true,
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],
  "angelscript.modules": [
    {
      "name": "MapInit",
      "entry": "${workspaceFolder}/maps/ins2/ins2_register.as"
    }
  ],
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

### Example C: Generic Game Engine

Use this for a standalone game or custom engine with its own include folder and API file.

```jsonc
{
  "angelscript.searchDirectories": [
    "${workspaceFolder}/scripts/include"
  ],
  "angelscript.modules": [
    {
      "name": "GameCore",
      "folder": "${workspaceFolder}/scripts/game"
    }
  ],
  "angelscript.predefinedFiles": [
    "${workspaceFolder}/scripts/api/engine.as.predefined"
  ]
}
```

## Predefined Game API Files (`.as.predefined`)

An `.as.predefined` file is a file that lists what the game gives to scripts (often called a stub). Your game registers classes, functions, enums, global variables, and `#define` flags in C++, and scripts call them at runtime. Listing them in a stub lets autocomplete, hover, and type checks understand your game's API.

### How stubs are loaded

- **Automatic scan**: Any file ending in `.as.predefined` inside `${workspaceFolder}` is detected automatically.
- **Pick one stub**: If you have more than one stub in your workspace, set `angelscript.predefined.active` to the file you want. When left empty, the editor loads the first file it finds.
- **Merge all stubs**: Set `angelscript.predefined.active` to `"all"` to load every discovered stub together.
- **External stubs**: Use `angelscript.predefinedFiles` when the stub lives outside `${workspaceFolder}`, or use path variables such as `${userHome}` and `${env:NAME}`.
- **Interactive switcher**: Click **AngelScript IntelliSense** in the status bar, or run **Select Predefined Stub** or **Select Predefined Stubs** from the Command Palette.

For Sven Co-op 5.26+, a maintained stub is included in this repository at [predefined/sven.as.predefined](../predefined/sven.as.predefined). You can follow the same format for any other game such as Trackmania Nations Forever or OpenSiv3D.

## Engine Rules (`*.angelscript.rules.json`)

Games often call functions such as `MapInit` or instantiate entity classes from C++ without calling them from another `.as` file. A `*.angelscript.rules.json` file tells the editor about those entry points, links delegate properties such as `BaseClass` to native stub types, and adds team or engine safety checks.

- **Automatic loading**: Place a `*.angelscript.rules.json` file in `${workspaceFolder}` or next to your active `.as.predefined` file.
- **Explicit path**: Set `angelscript.rulesFile` in `.vscode/settings.json` to point to a specific rules file.
- **Generate a template**: Run **Configure Host Rules & Predefined API...** from the Command Palette.

For field-by-field examples and editor behavior, see the [Engine Rules Guide](rules.md) and the reference Sven Co-op rules at [predefined/sven.angelscript.rules.json](../predefined/sven.angelscript.rules.json).

## Engine Options and What Changes in the Editor

Each game configures AngelScript with its own language rules. The settings below mirror those engine flags (`asEP_*`) and preprocessor behaviors.

### Language and Syntax Options (`angelscript.engine.*`)

| Setting | Default | What changes in the editor |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | When `false`, parameters like `int &x` show an error asking for `&in` or `&out`. When `true`, plain `&` is accepted on primitive and value types. |
| `angelscript.engine.privatePropAsProtected` | `false` | When `false`, reading a parent class's `private` member from a child class reports an access error. When `true`, child classes can access it and see it in autocomplete. |
| `angelscript.engine.disallowGlobalVars` | `false` | When `true`, any variable declared outside a function or class is flagged as an error. When `false`, global variables are allowed. |
| `angelscript.engine.propertyAccessorMode` | `2` | With `2`, any `get_X()` or `set_X(v)` method acts as property `X` in autocomplete and type checks. With `3`, only methods marked with `property` work as properties, and others report an unknown member error. |
| `angelscript.engine.allowMultilineStrings` | `false` | When `false`, line breaks inside `"..."` strings show an error and require `"""..."""`. When `true`, multi-line `"..."` strings are accepted. |
| `angelscript.engine.boolConversionMode` | `0` | With `0`, using a class in `if (obj)` reports an error even if it defines a `bool` conversion. With `1`, classes with `opImplConv` or `opConv` returning `bool` are accepted in conditions. |
| `angelscript.engine.useCharacterLiterals` | `0` | With `0`, `'A'` is treated as a `string`, so assigning `'A'` to `int` shows a type error. With `1`, `'A'` is treated as an integer character code. |
| `angelscript.engine.disallowValueAssignForRef` | `false` | When `true`, `a = b` on a handle-capable class shows an error and requires handle assignment `@a = @b`. When `false`, `a = b` resolves against `opAssign`. |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | With `0`, named arguments must use `name: value` and `name = value` is an error. With `1`, `name = value` shows a warning. With `2`, both forms are accepted without warnings. |
| `angelscript.engine.disableIntegerDivision` | `false` | When `false`, `1 / 2` stays an integer type. When `true`, `int / int` produces a floating-point type, so assigning it to an `int` triggers a conversion diagnostic. |
| `angelscript.engine.disallowEmptyListElements` | `false` | When `true`, empty slots in `{1, , 3}` show an error. When `false`, empty slots in initializer lists are allowed. |
| `angelscript.engine.foreachSupport` | `true` | When `true`, `foreach` loops parse and infer the loop variable type. When `false`, every `foreach` loop shows an unsupported feature error. |
| `angelscript.engine.requireEnumScope` | `false` | When `true`, enum values like `Red` must be written as `Color::Red`, bare names report an error, and autocomplete inserts the enum prefix. When `false`, bare enum names are accepted. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | When `false`, adding a constructor like `Item(int id)` hides the zero-argument constructor so `Item x;` shows an error unless `Item()` is written. When `true`, `Item x;` is always accepted. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | When `false`, non-ASCII letters in variable or function names show syntax errors. When `true`, UTF-8 letters are accepted in identifiers. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | When `false`, repeating the same `shared interface` across files reports a duplicate declaration error. When `true`, identical definitions are merged without error. |
| `angelscript.engine.compilerWarnings` | `1` | `0` hides compiler warnings, `1` shows them as warning squiggles, and `2` turns every compiler warning into a red error. |

### Preprocessor and Metadata Options

| Setting | Default | What changes in the editor |
| :--- | :--- | :--- |
| `angelscript.preprocessor.elseSupport` | `false` | When `false`, `#else` is ignored and lines after a false `#if` stay dimmed and skipped until `#endif`. When `true`, `#else` flips to the active branch when `#if` is false. |
| `angelscript.preprocessor.elifSupport` | `false` | When `true`, `#elif` chains are evaluated and only the first matching branch stays active instead of dimmed. |
| `angelscript.preprocessor.ifdefSupport` | `false` | When `false`, `#ifdef` and `#ifndef` show syntax errors. When `true`, they toggle active and dimmed blocks using `angelscript.define` and stub `#define` lines. |
| `angelscript.preprocessor.defineInScripts` | `false` | When `false`, `#define WORD` inside `.as` scripts shows a syntax error. When `true`, script `#define` lines are accepted and turn on matching `#if` blocks. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | Controls `#pragma` diagnostics: `"accept"` reports nothing, `"hint"` shows a hint, and `"error"` flags `#pragma` as an error. |
| `angelscript.metadata.enabled` | `false` | When `true`, `[...]` metadata blocks before classes, interfaces, functions, and variables parse without syntax errors. |

## Formatting and `.clang-format`

Use `angelscript.format.*` settings for quick style changes such as brace placement and spacing. If your workspace or a parent folder contains a `.clang-format`, `_clang-format`, or `.as-clang-format` file, the formatter reads it automatically and applies those rules instead. Supported options include `BasedOnStyle` presets, `Language: AngelScript` or `Cpp` sections, `BraceWrapping`, `SpaceBeforeParens`, `PointerAlignment`, `ShortBlocks`, `ReflowComments`, and `// clang-format off` / `// clang-format on`.

## Path Variables

Path settings accept these variables:

| Variable | Expands to |
| :--- | :--- |
| `${workspaceFolder}` | Root folder of the active workspace. |
| `${workspaceFolder:name}` | Root folder of a named workspace in a multi-root setup. |
| `${userHome}` | Current user's home folder. |
| `${env:NAME}` | Value of the environment variable `NAME`. |

## Contributed Commands

Open the Command Palette (`Ctrl+Shift+P` or `Cmd+Shift+P`) or right-click files and folders in the Explorer:

| Command | Title | What it does |
| :--- | :--- | :--- |
| `angelscript.configureHostEnvironment` | **AngelScript: Configure Host Rules & Predefined API...** | Creates a rules template for your workspace. |
| `angelscript.ignoreUnusedSymbol` | **AngelScript: Ignore Unused Symbol (Mark as Entry Point / Callback)** | Quick fix that adds a symbol to your rules exemptions. |
| `angelscript.selectPredefined` | **AngelScript: Select Predefined Stub** | Switches the active `.as.predefined` file. |
| `angelscript.selectStubs` | **AngelScript: Select Predefined Stubs** | Picks and merges multiple `.as.predefined` files. |
| `angelscript.rescanWorkspace` | **AngelScript: Rescan Workspace** | Re-indexes all scripts and stubs in the workspace. |
| `angelscript.statusMenu` | **AngelScript: AngelScript Status Menu** | Opens the status menu with active module and stub actions. |
| `angelscript.switchActiveModule` | **AngelScript: Switch Active Module** | Switches which module is active in the editor. |
| `angelscript.showServerLog` | **AngelScript: Show Server Log** | Opens the language server output channel. |
| `angelscript.openLogsFolder` | **AngelScript: Open Logs Folder** | Opens the folder containing server log files. |
| `angelscript.restartServer` | **AngelScript: Restart Server** | Restarts the language server process. |
| `angelscript.setModuleEntryPoint` | **Set as Module Entry Point** | Explorer context menu on `.as` files to set a module entry point. |
| `angelscript.setModuleFolder` | **Set as Module Folder** | Explorer context menu on folders to assign folder module ownership. |
| `angelscript.formatPredefinedStub` | **Format Predefined Stub** | Explorer context menu on `.predefined` files to format API headers. |
| `angelscript.clearLocalState` | **Clear Local Session Overrides** | Clears choices saved in local workspace session state. |

## Debugger

The extension includes a VS Code debugger (`type: "angelscript"`) with breakpoint validation, call stack view, and local variable inspection. You can connect over `dap`, `asdbg`, or `cdebugger` using `angelscript.debug.defaultProtocol` and `angelscript.debug.defaultPort` (default `9002`).

## Building from Source

Prerequisites:
- C++20 compiler: MSVC 2022 on Windows, GCC 13+ or Clang 16+ on Linux or macOS.
- CMake 3.22+ and Ninja (recommended).
- Node.js 18+ and `npm` for the VS Code client.

```bash
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
cd client && npm install && npm run compile
```

## Full Settings Catalog

<!-- SETTINGS_CATALOG_START -->
#### 1. General & Server Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.server.executablePath` | `""` | Path to the AngelScript Language Server executable. |
| `angelscript.server.logLevel` | `"debug"` | Specifies the verbosity level for language server logging output. |
| `angelscript.dimInactiveRegions` | `true` | Dim the code inside a `#if` block the preprocessor drops, the way the C++ extension dims its inactive regions. A decoration rather than a colour: it dims whatever the syntax highlighting produced, brackets included, which a semantic token cannot do - the editor paints `(`, `{` and `[` from its own bracket-pair feature. |
| `angelscript.inactiveRegionOpacity` | `0.55` | How visible the dimmed code inside a dropped `#if` block stays. 1 is no dimming at all. Matches `#angelscript.dimInactiveRegions#`, which has to be on for this to do anything. |

#### 2. Workspace & Script Modules Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.configurationTarget` | `"workspace"` | Specifies where commands like 'Set Module Entry Point' or 'Select Predefined Stub' store their choices: in the workspace file (.vscode/settings.json), in local workspace session state (context.workspaceState, avoiding Git changes), or in global user settings. |
| `angelscript.modules` | `[]` | The script modules this workspace builds, as `{ "name": "...", "entry": "..." }` entries. A module is AngelScript's own unit of compilation, and the entry is the `.as` the host builds it from - everything that file reaches through `#include` belongs to it.  Empty by default, and empty changes nothing. It exists because `external shared class Foo;` compiles only when *another* module declares `shared class Foo` - measured, and with the definition in the same module the compiler still rejects it. Without knowing the modules the server cannot tell a correct declaration from a broken one, so it stays quiet; with them, it can say.  The `entry` accepts the same `${...}` variables as every other path setting. An entry may name a `folder` instead of, or as well as, an `entry` - a folder module owns every script under it, and the deepest folder wins when they nest. |
| `angelscript.moduleEntryPoint` | `""` | Global module entry-point script path. Restricts compilation closure to its forward dependency DAG. |
| `angelscript.searchDirectories` | `[]` | Custom directory search paths for resolving included script files (`#include "path.as"`).  Accepts the `${...}` variables `launch.json` uses: `${workspaceFolder}`, `${workspaceFolder:name}`, `${userHome}` and `${env:NAME}`. VS Code does not expand these in ordinary settings, so the extension does it - a variable this window cannot answer is left in the path as written and noted in the server log. |
| `angelscript.predefined.active` | `""` | Selects the single predefined stub loaded by the workspace scan.  When empty (the default) the scan loads the first stub it finds in path order and ignores the rest, and says which one it chose. Set it to `all` to load every discovered stub together, which is what a workspace with two stubs used to do by default - shared declarations then resolve more than once.  Does not affect `#angelscript.predefinedFiles`, which continue to load always.  The "AngelScript: Select Predefined Stub" command populates this setting for you.  Accepts the `${...}` variables `launch.json` uses: `${workspaceFolder}`, `${workspaceFolder:name}`, `${userHome}` and `${env:NAME}`. VS Code does not expand these in ordinary settings, so the extension does it - a variable this window cannot answer is left in the path as written and noted in the server log. |
| `angelscript.predefinedFiles` | `[]` | Predefined stub files declaring the host application's API, loaded by path. Absolute paths are used as-is, so the stub can live outside the workspace (e.g. `C:/Games/svencoop/as.predefined`); relative paths are resolved against each workspace folder. Files inside the workspace whose name ends with `#angelscript.predefinedExtension` are picked up automatically and need no entry here.  Accepts the `${...}` variables `launch.json` uses: `${workspaceFolder}`, `${workspaceFolder:name}`, `${userHome}` and `${env:NAME}`. VS Code does not expand these in ordinary settings, so the extension does it - a variable this window cannot answer is left in the path as written and noted in the server log. |
| `angelscript.stubs.activeFiles` | `[]` | List of active predefined stub files loaded into the workspace index. Managed dynamically via the AngelScript: Select Predefined Stubs command. |
| `angelscript.rulesFile` | `""` | Path to a custom *.angelscript.rules.json engine rules file. When empty, the workspace root and predefined stub directories are automatically scanned for *.angelscript.rules.json. |
| `angelscript.include.implicitExtension` | `false` | Let `#include "helper"` find `helper.as`. Off by default, which is AngelScript's own behaviour: CScriptBuilder opens exactly the text between the quotes and appends nothing, so `#include "helper"` looks for a file literally named `helper`. Turn it on for a host that resolves the name itself and requires the extension to be left off - Sven Co-op works this way, and there the short form is the correct spelling. The suffix appended is `angelscript.fileExtension`. |
| `angelscript.forceIncludeFiles` | `[]` | Files to force-include before analyzing any module or script. |
| `angelscript.exclude` | `["**/.git/**", "**/build/**", "**/node_modules/**"]` | Directory globs the workspace scans do not descend into. Supports `?`, `*` within a path segment and `**` across segments. Setting this replaces the defaults rather than adding to them. |
| `angelscript.fileExtension` | `".as"` | Filename suffix of AngelScript source files, used when scanning the workspace to build the `#include` graph. |
| `angelscript.predefinedExtension` | `".as.predefined"` | Filename suffix that marks a file found in the workspace as a predefined stub. This is a suffix, not a path - use `#angelscript.predefinedFiles` to load a specific file. |
| `angelscript.define` | `[]` | Words `#if` treats as defined, matching the words the host application passes to `CScriptBuilder::DefineWord`. A `#if WORD` whose word is not listed here is excluded from compilation, exactly as it is by the real preprocessor, and no diagnostics are reported inside it. A predefined stub can declare the same words with `#define WORD`, which is usually the better place for them: the stub already describes the host's engine setup and travels with it. |
| `angelscript.arrayLikeTypes` | `[]` | Names of template types whose initializer list is a plain repeat of their element type, the way `array<T>`'s is. The engine's default array type is always included.  This is shorthand. The general mechanism is a `/// @listpattern {repeat T}` tag on the class in your `.as.predefined` stub, copied from the type's own `asBEHAVE_LIST_FACTORY` registration — that also expresses patterns this setting cannot, such as `dictionary`'s `{repeat {string, ?}}`. Use this setting when you cannot edit the stub.  Either way it has to be stated rather than detected: `array<T>` and `optional<T>` are declared identically in a stub, and the compiler accepts `array<int> a = {1};` while rejecting `optional<int> o = {1};`. |

#### 3. Inlay Hints Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.features.inlayHints` | `true` | Show inline parameter-name and deduced-type hints. |
| `angelscript.inlayHints.parameters` | `true` | Show parameter name hints for function calls and constructors. |
| `angelscript.inlayHints.variableTypes` | `true` | Show deduced type hints for variables declared with 'auto' or 'auto@'. |
| `angelscript.inlayHints.functionReturnTypes` | `true` | Show inlay hints for inferred return types on lambda expressions. |
| `angelscript.inlayHints.enumMemberValues` | `true` | Show inlay hints for implicit numeric values on enum members. |
| `angelscript.inlayHints.parameterReferenceModifiers` | `true` | Prefix parameter inlay hints with reference direction modifiers (&out, &inout). |
| `angelscript.inlayHints.maxParameters` | `0` | Maximum number of parameter inlay hints to display for a call. 0 means unlimited (show all parameters). |
| `angelscript.inlayHints.maxLength` | `0` | Maximum character length for parameter inlay hint labels before truncating with '...'. 0 means unlimited (never truncate). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `true` | Suppress parameter name hints when the argument expression text matches the parameter name exactly. Default is true. |
| `angelscript.inlayHints.enableTooltip` | `true` | Show rich markdown tooltips with type and parameter signatures when hovering over inlay hints. |
| `angelscript.inlayHints.enableLocation` | `true` | Enable Ctrl+Click navigation to parameter declarations from inlay hints. |
| `angelscript.inlayHints.omittedDefaultArguments` | `"off"` | Show inlay hints for omitted optional arguments with default values in function calls. |

#### 4. Code Formatting Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.features.formatting` | `true` | Enable formatting services (document formatting, range formatting, on-type formatting, and format on save). When disabled, no code formatting will occur. |
| `angelscript.features.onTypeFormatting` | `false` | Automatically format code on typing specific trigger characters (semicolon, closing brace, newline). |
| `angelscript.format.onSave` | `false` | Format the whole document when you save it manually.  Off by default: your editor already has `editor.formatOnSave`, and a language server that reformats every save regardless would override that choice silently. Autosave and focus-change saves never format, whatever this is set to - rewriting a file while you are still typing in it is not on offer. |
| `angelscript.format.braceStyle` | `"allman"` | Where a block's opening brace goes. An initializer list and a lambda body keep their brace on the line under either style. |
| `angelscript.format.spacesInsideParentheses` | `false` | Whether to insert spaces inside parentheses (e.g. 'foo( bar )' instead of 'foo(bar)'). |
| `angelscript.format.keepEmptyBlocksOnSingleLine` | `true` | Keep empty blocks on a single line (e.g. 'ClassName() {}') rather than expanding them across multiple lines. |
| `angelscript.format.pointerAlignment` | `"left"` | Alignment of handle (`@`) and reference (`&`) qualifiers in declarations and parameters. Overridden when a `.clang-format`, `_clang-format`, or `.as-clang-format` file is present in the workspace. |

#### 5. Language Features & Autocompletion Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.features.hover` | `true` | Show type and documentation tooltips on hover. |
| `angelscript.hover.stringLiteralLength` | `true` | Show the character length of string literals in hover tooltips. |
| `angelscript.hover.stringLiteralPathResolution` | `true` | Probe and resolve string literals that look like file/asset paths against the document directory, workspace roots, and configured asset search paths. |
| `angelscript.hover.assetSearchPaths` | `[]` | Directories to search for asset files referenced in string literals when path resolution is enabled. |
| `angelscript.hover.showFileLocation` | `false` | Show the declaring file location footer in symbol hover tooltips (default: false). |
| `angelscript.features.completion` | `true` | Enable auto-completion suggestions. |
| `angelscript.completion.smartTypeRanking` | `true` | Prioritize autocompletion items matching expected parameter or assignment target types. |
| `angelscript.completion.completeFunctionParens` | `true` | Automatically append parentheses and position cursor when completing functions or methods. |
| `angelscript.completion.qualifyEnumValues` | `true` | Automatically prefix enum name when completing enum values (e.g. inserting 'EnumName::EnumValue') to eliminate ambiguity. |
| `angelscript.features.definition` | `true` | Enable Go to Definition and Go to Type Definition. |
| `angelscript.features.references` | `true` | Enable Find All References. |
| `angelscript.features.signatureHelp` | `true` | Show parameter hints while typing a call. |
| `angelscript.features.semanticTokens` | `true` | Enable semantic syntax highlighting. |
| `angelscript.features.documentSymbols` | `true` | Populate the Outline view and breadcrumbs. |
| `angelscript.features.workspaceSymbols` | `true` | Enable workspace-wide symbol search (Ctrl+T). |
| `angelscript.features.rename` | `true` | Enable symbol rename. |
| `angelscript.features.documentHighlight` | `true` | Highlight other occurrences of the symbol under the cursor. |
| `angelscript.features.foldingRange` | `true` | Provide code folding regions. |
| `angelscript.features.codeAction` | `true` | Offer quick fixes and refactorings. |
| `angelscript.features.documentLink` | `true` | Turn `#include` directives into clickable links. |
| `angelscript.features.implementation` | `true` | Go to Implementation: from an interface or a base class to the types that answer to it, and from a method to the ones that implement or override it. |
| `angelscript.features.selectionRange` | `true` | Expand selection: grow the selection one syntactic step at a time. |
| `angelscript.features.callHierarchy` | `true` | Call hierarchy: who calls this function, and what it calls in turn. |
| `angelscript.features.typeHierarchy` | `true` | Type hierarchy: the bases a class or interface declares, and the types that declare it as theirs. |
| `angelscript.features.linkedEditing` | `true` | Linked editing: retype a local variable or a parameter and its uses together, live. Offered only for names a lexical scope keeps inside one file - anything at file scope goes through Rename instead, which looks across documents. |
| `angelscript.features.codeLens` | `true` | Show actionable code lenses (references, implementations) inline above declarations. |
| `angelscript.features.typeConversionChecks` | `true` | Report conversions with no constructor, `opConv`/`opImplConv` or `opCast`/`opImplCast` to back them. |
| `angelscript.features.predefinedLoader` | `true` | Load predefined stub files describing the host application's API. |
| `angelscript.features.enableCommentSuppressions` | `true` | Enable comment-based diagnostic suppressions using '// disable <CODE>' and '// enable <CODE>' (e.g. '// disable W156'). |

#### AngelScript: CodeLens

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.codeLens.showOnFunctions` | `true` | Show reference CodeLens above function and method declarations. |
| `angelscript.codeLens.showOnClassesAndInterfaces` | `true` | Show reference CodeLens above class and interface declarations. |
| `angelscript.codeLens.showOnPropertiesAndVariables` | `true` | Show reference CodeLens above global and member variable declarations. |
| `angelscript.codeLens.showOnEnums` | `true` | Show reference CodeLens above enum declarations. |
| `angelscript.codeLens.showZeroReferences` | `true` | Show reference CodeLens even when a symbol has 0 references. |

#### 6. Semantic Diagnostics & Analysis Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.diagnosticSeverity` | `{}` | Overrides the severity of individual diagnostics, keyed by diagnostic code. Example: `{ "as-warn-unused-variable": "hint" }`. |
| `angelscript.diagnostics.reportUnknownTypes` | `true` | Report a parameter or return type that resolves to no declaration.  `void f(TypoTypeName x)` is a compile error — the engine answers "Identifier 'TypoTypeName' is not a data type in namespace 'TEST' or parent" — and left unreported it surfaces as silence at every call site instead, because a call whose parameter types are unknown cannot be checked either.  It is also, from the server's side, indistinguishable from a legitimate engine-registered type. Turn it off if your host registers types in C++ and declares none of them — or better, provide a predefined stub file via `#angelscript.predefinedFiles`. |
| `angelscript.diagnostics.reportAccessorPortability` | `false` | Hint on a `get_`/`set_` accessor written without the `property` keyword. Such an accessor is a property under `asEP_PROPERTY_ACCESSOR_MODE` 2, this server's default, but not under mode 3, which is the engine's own. Adding the keyword is accepted under both, so the quick fix cannot break a working build. Disabled by default. |
| `angelscript.diagnostics.reportAccessorDisabled` | `true` | Hint where a script property accessor is used as a property but the host disabled those (`angelscript.engine.propertyAccessorMode` 0 or 1).  Under either mode the compiler skips script-defined accessors entirely, so `c.X` backed by a script `get_X`/`set_X` is rejected — with the `property` keyword and without it. A hint rather than an error, and off by default, because the analyzer is being told what the host does: a host told wrong would otherwise see errors on code that builds for it. Says nothing under modes 2 and 3. |
| `angelscript.diagnostics.reportBoolConversion` | `true` | Hint when a class is used where a `bool` is expected, such as `if (h)`. Under `asEP_BOOL_CONVERSION_MODE` 0, the engine's default, this is a compile error even when the class declares `opImplConv`. A quick fix calls the conversion operator explicitly, which compiles under both modes. Off by default, and silent entirely when `angelscript.engine.boolConversionMode` is 1. |
| `angelscript.diagnostics.reportMissingFuncdef` | `false` | Hint when a type position names a function rather than a type, such as `Foo@ h` where `Foo` is a function. A function handle needs a `funcdef` to name its signature, and a quick fix declares one from the function's own parameters and return type. Off by default, since the name could belong to a host type this analyzer cannot see. |
| `angelscript.diagnostics.reportIntegerDivision` | `false` | Hint on `1 / 2`, which truncates to 0 under the engine's default. Off by default because a codebase that means integer division writes exactly this. |
| `angelscript.diagnostics.reportPossibleNullDereference` | `true` | Warn when a handle parameter, handle cast result, or uninitialized handle is dereferenced without a preceding null check (`!is null`, early guard return, etc.). Enabled by default. |
| `angelscript.diagnostics.reportUnusedFunctions` | `false` | Warn when a global function, namespaced function, or private class method is declared in the script but never called or referenced. Host entry points can be configured via `#angelscript.diagnostics.ignoredUnusedFunctions#` or engine rules (`.angelscript.rules.json`). |
| `angelscript.diagnostics.reportUnusedGlobalVariables` | `true` | Warn when a global or namespace-level variable is declared but never referenced in any script. Enabled by default. |
| `angelscript.diagnostics.reportUnusedClasses` | `false` | Warn when a class is declared in the script but never instantiated, inherited, or referenced. Disabled by default. |
| `angelscript.diagnostics.reportUnusedMembers` | `false` | Warn when a class member variable or method is declared but never accessed or called in any script. |
| `angelscript.diagnostics.ignoredUnusedFunctions` | `[]` | Function names (short or qualified) exempt from unused-function warnings (e.g., `["main", "MapInit", "PluginInit"]`). |
| `angelscript.diagnostics.ignoredUnusedFunctionPatterns` | `[]` | Regular expression patterns exempting matching function names from unused-function warnings (e.g., `["^On.*", "^Hook_.*"]`). |
| `angelscript.diagnostics.ignoreUnusedGlobalsWithCallInitializer` | `false` | Whether to ignore unused global variables initialized with a call or constructor expression. |
| `angelscript.diagnostics.reportHandleComparisonEquality` | `1` | Configure severity for handle equality comparisons with null (`== null`, `!= null`) instead of handle identity (`is null`, `!is null`). 0: disabled, 1: warning (compiler default), 2: error. |
| `angelscript.diagnostics.missingAssetPathSeverity` | `"off"` | Diagnostic severity for string literals referencing missing asset files. Off by default. |

#### 7. Engine Dialect & Preprocessor Configuration (asEP_*)

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Whether parameters may take references to primitive and value types without explicit direction qualifiers (e.g. `void fn(int &val)` or `void fn(const string &msg)`).  When disabled (the engine default), reference parameters on primitives and value types require `&in` or `&out` (e.g. `void fn(int &in val)`), and plain `&` is treated as `&inout` which produces an error. When enabled, raw C++-style references `&` are permitted for all types.  (Matches host engine property `asEP_ALLOW_UNSAFE_REFERENCES`). |
| `angelscript.engine.privatePropAsProtected` | `false` | Whether `private` members in a base class behave as `protected` and can be accessed by derived classes (e.g. `derived.m_privateField`).  When enabled, derived classes can access private members of base classes without error. When disabled (the engine default), accessing a private base member triggers an accessibility error.  (Matches host engine property `asEP_PRIVATE_PROP_AS_PROTECTED`). |
| `angelscript.engine.disallowGlobalVars` | `false` | Whether global variable declarations in scripts are forbidden.  When enabled, declaring any global variable (e.g. `int g_counter = 0;`) outside of a function or class produces a compile error. When disabled (the engine default), global variables are permitted.  (Matches host engine property `asEP_DISALLOW_GLOBAL_VARS`). |
| `angelscript.engine.propertyAccessorMode` | `2` | Controls how getter and setter methods (`get_Prop()`, `set_Prop(val)`) are exposed as virtual properties (e.g. `obj.Prop`).  `2` (Any member) — Any method matching `get_X()` or `set_X(...)` can be accessed as property `X`. `3` (Require property keyword) — Only methods explicitly declared with the `property` keyword (e.g. `int get_Prop() property`) are exposed as properties; referencing `obj.Prop` otherwise triggers an unknown member error.  The engine's own default is `3`; the LSP default is `2` for maximum compatibility.  (Matches host engine property `asEP_PROPERTY_ACCESSOR_MODE`). |
| `angelscript.engine.allowMultilineStrings` | `false` | Whether standard double-quoted strings `"..."` are allowed to span multiple lines.  When disabled (the engine default), a newline inside `"..."` triggers a compile error, and multiline strings must use triple quotes `"""..."""`. When enabled, regular `"..."` strings may freely span multiple lines.  (Matches host engine property `asEP_ALLOW_MULTILINE_STRINGS`). |
| `angelscript.engine.boolConversionMode` | `0` | Controls whether class instances and handles can be evaluated directly in boolean conditions (e.g. `if (myObj)`).  `0` (Disallow class conversions) — Classes cannot be used as conditions, even if they declare `opImplConv` or `opConv` to `bool`. `1` (Allow conversion operators) — Classes implementing `opImplConv` or `opConv` returning `bool` are accepted in boolean expressions.  (Matches host engine property `asEP_BOOL_CONVERSION_MODE`). |
| `angelscript.engine.useCharacterLiterals` | `0` | Controls how single-quoted character literals `'x'` are interpreted by the compiler.  `0` (Single-character string) — `'x'` is treated as a 1-character string literal (the engine default), making `int c = 'x'` a type mismatch error. `1` (Character code integer) — `'x'` is treated as an ASCII/integer character code (e.g. `int c = 'A'` evaluates to 65).  (Matches host engine property `asEP_USE_CHARACTER_LITERALS`). |
| `angelscript.engine.disallowValueAssignForRef` | `false` | Controls whether value assignment using `=` is forbidden on reference types.  When enabled, value assignment `a = b` on reference objects is rejected as an error, requiring handle assignment `@a = @b` instead. When disabled (the engine default), `a = b` invokes the copy assignment operator `opAssign` on the object.  (Matches host engine property `asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE`). |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | Syntax permitted for named function call arguments (e.g. `draw(x: 10, y: 20)` vs `draw(x = 10, y = 20)`).  `0` (Strict colon syntax) — Only `name: value` is accepted (the engine default); `name = value` is a syntax error. `1` (Equal sign with warning) — `name = value` is accepted but generates a compiler warning. `2` (Equal sign accepted silently) — Both `name: value` and `name = value` are fully valid without diagnostics.  (Matches host engine property `asEP_ALTER_SYNTAX_NAMED_ARGS`). |
| `angelscript.engine.disableIntegerDivision` | `false` | Controls whether dividing two integers with `/` performs floating-point division instead of integer truncation.  When enabled, integer division produces a float (e.g. `1 / 2` evaluates to `0.5f`). When disabled (the engine default), integer division truncates toward zero (e.g. `1 / 2` evaluates to `0`).  (Matches host engine property `asEP_DISABLE_INTEGER_DIVISION`). |
| `angelscript.engine.disallowEmptyListElements` | `false` | Whether omitting elements with empty commas in initialization lists (e.g. `{1, , 3}`) is treated as an error.  When enabled, skipped slots in array and object initializer lists produce a compile error. When disabled (the engine default), empty slots are permitted and initialized with default values.  (Matches host engine property `asEP_DISALLOW_EMPTY_LIST_ELEMENTS`). |
| `angelscript.engine.foreachSupport` | `true` | Controls whether `foreach` iteration loops (e.g. `foreach (auto item : container)`) are supported.  When enabled (the engine default), `foreach` loops compile normally. When disabled, encountering a `foreach` statement produces a syntax or unsupported feature error.  (Matches host engine property `asEP_FOREACH_SUPPORT`). |
| `angelscript.engine.requireEnumScope` | `false` | Controls whether enum values must always be prefixed with their enum type name (e.g. `Color::Red` vs plain `Red`).  When enabled, bare enumerator names are not visible in the enclosing scope, and `Red` produces an unresolved symbol error unless written as `Color::Red`. When disabled (the engine default), enumerator values can be used directly without scope qualification.  (Matches host engine property `asEP_REQUIRE_ENUM_SCOPE`). |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | Controls whether classes automatically provide a default zero-argument constructor when custom constructors are defined.  When enabled (the engine default), an implicit default constructor `MyClass()` is always generated, allowing `MyClass obj;` even if `MyClass(int a)` exists. When disabled, defining any constructor prevents default construction unless an explicit `MyClass()` constructor is written.  (Matches host engine property `asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`). |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | Whether Unicode and non-ASCII characters are allowed in identifier and symbol names (e.g. `int счётчик = 0;` or `void calcular_año()`).  When enabled, UTF-8 non-ASCII letters and digits are valid in variable, function, and class names. When disabled (the engine default), non-ASCII characters in identifiers produce syntax errors.  (Matches host engine property `asEP_ALLOW_UNICODE_IDENTIFIERS`). |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | Whether declaring the same shared interface in multiple script files compiles cleanly without duplicate declaration errors.  When enabled, identically declared `shared interface IFoo` across different modules or files are merged without error. When disabled (the engine default), re-declaring the same shared interface produces a name conflict diagnostic.  (Matches host engine property `asEP_IGNORE_DUPLICATE_SHARED_INTF`). |
| `angelscript.engine.compilerWarnings` | `1` | Controls how the engine and language server handle compiler warnings.  `0` (Suppress warnings) — Disables compiler warning reporting completely. `1` (Emit warnings) — Reports compiler warnings as LSP warning diagnostics (the engine default). `2` (Treat warnings as errors) — Promotes compiler warnings to compilation errors, blocking successful builds.  (Matches host engine property `asEP_COMPILER_WARNINGS`). |
| `angelscript.preprocessor.elseSupport` | `false` | Set this if the host patched its copy of `scriptbuilder.cpp` to understand `#else`.  The add-on shipped with the SDK does not. Measured against the real compiler: in `#if FOO / a / #else / b / #endif` with `FOO` undefined, the `#else` branch does **not** become the taken one — it is blanked along with the rest of the block, because `#else` is not a directive and the exclusion runs to the `#endif` regardless. With this off, that is exactly what this server assumes. |
| `angelscript.preprocessor.elifSupport` | `false` | Set this if the host patched its copy of `scriptbuilder.cpp` to understand `#elif`.  Not in the stock add-on. With it on, the first branch whose word is defined is the live one and every other branch is excluded. |
| `angelscript.preprocessor.ifdefSupport` | `false` | Set this if the host patched its copy of `scriptbuilder.cpp` to understand `#ifdef` and `#ifndef`.  Not in the stock add-on, where either one is left in the source and the compiler reports `Unexpected token`. |
| `angelscript.preprocessor.defineInScripts` | `false` | Set this if the host patched its copy of `scriptbuilder.cpp` so `#define WORD` in a script defines a word.  Not in the stock add-on, where `DefineWord` is a C++ call the host makes and a `#define` written in a script is a syntax error. To declare the words the host itself defines, use `#angelscript.define` or a `#define` line in a predefined stub, which are not affected by this setting. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | What to report for a `#pragma`.  The stock add-on rejects every one: with no pragma callback registered it substitutes a failure for the callback's answer, writes `Invalid #pragma directive` and fails the whole section. The default here is nevertheless `accept`, because a host that registers a callback is the common case and reporting an error by default would put a squiggle on a pragma that builds fine. Choose `error` for a host that really registered nothing, or `hint` if you are not sure. |
| `angelscript.metadata.enabled` | `false` | Enable support for CScriptBuilder metadata blocks ('[...]') before script declarations (classes, interfaces, functions, variables). |

#### AngelScript: Debugger

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.debug.enableBreakpointLocations` | `true` | Enable semantic breakpoint location verification via AngelLSP server before binding breakpoints. |
| `angelscript.debug.defaultPort` | `9002` | Default TCP port for connecting to AngelScript runtime debug server (default: 9002). |
| `angelscript.debug.defaultProtocol` | `"dap"` | Default protocol for AngelScript debugging ('dap' for Debug Adapter Protocol, 'asdbg' for binary socket, or 'cdebugger' for text command socket). |
<!-- SETTINGS_CATALOG_END -->

---

See also: [README](../README.md) | [Engine Rules Guide](rules.md) | [CHANGELOG.md](../CHANGELOG.md)
