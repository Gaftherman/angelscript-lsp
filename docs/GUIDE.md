# Angelscript Detailed Guide

[English](GUIDE.md) | [Español](GUIDE.es.md) | [README](../README.md)

Start with a workspace example that matches how your game loads scripts. Then use the settings below to tell VS Code about the game’s API and language rules.

## Workspace Setup Examples

Put the example in `.vscode/settings.json` under `${workspaceFolder}`. In `angelscript.modules`, use `folder` for every script under a directory or `entry` for one script and the files it reaches through `#include`.

Module boundaries let the editor check whether an `external shared` declaration has a matching definition in another module.

### Example A: Sven Co-op (Folder Module)

If all scripts under `maps/hcas` build together, give that folder a module name.

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

If your game starts from `ins2_register.as`, use that file as the entry point. Its includes join the same module.

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

If your engine keeps shared includes and API declarations in separate directories, give each directory or file its own path in the settings.

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

Add an `.as.predefined` stub to describe the API your host registers in C++. The editor can then suggest the host’s classes and functions, show their signatures on hover, and check how you use them.

### How stubs are loaded

- The workspace scan looks for files with the `angelscript.predefinedExtension` suffix, which defaults to `.as.predefined`.
- Set `angelscript.predefined.active` to the stub you want. Leave it empty to use the first one in path order, or set it to `"all"` to load every stub the scan finds.
- List stubs you always want to load in `angelscript.predefinedFiles`. They can live outside `${workspaceFolder}`. Paths can use `${userHome}` and `${env:NAME}`.
- Run **Select Predefined Stub** or **Select Predefined Stubs** from the Command Palette. You can also open the actions menu from the **AngelScript IntelliSense** language status item.

For a Sven Co-op 5.26 example, open [predefined/sven.as.predefined](../predefined/sven.as.predefined).

## Engine Rules (`*.angelscript.rules.json`)

If your engine calls `MapInit` from C++, add it to the engine rules to clear the unused-function warning. You can also use rules to link `BaseClass` to a native class in a stub or flag calls and declarations that break your host’s rules.

- Put `angelscript.rules.json` at the workspace root for automatic discovery. The server also looks for `*.angelscript.rules.json` files beside stubs and at the root.
- Set `angelscript.rulesFile` in `.vscode/settings.json` to choose a specific file.
- To generate a starter file, run **Configure Host Rules & Predefined API...** in the Command Palette and choose the starter rules option.

For examples of each field, see the [Engine Rules Guide](RULES.md) and [predefined/sven.angelscript.rules.json](../predefined/sven.angelscript.rules.json).

## Engine Options and What Changes in the Editor

Match these settings to the `asEP_*` flags and preprocessor behavior your host uses. The defaults below are the extension’s defaults.

### Language and Syntax Options (`angelscript.engine.*`)

| Setting | Default | What changes in the editor |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Set this to `true` if your host accepts plain `&` for primitive and value parameters such as `int &x`. Otherwise, use a direction such as `&in` or `&out`. |
| `angelscript.engine.privatePropAsProtected` | `false` | With `true`, a derived class can access a base class’s `private` members. |
| `angelscript.engine.disallowGlobalVars` | `false` | With `true`, the editor flags declarations of global variables. |
| `angelscript.engine.propertyAccessorMode` | `2` | With `2`, `get_X()` and `set_X(v)` can expose `obj.X`. With `3`, declare the accessors with `property` to use `obj.X`. |
| `angelscript.engine.allowMultilineStrings` | `false` | With `true`, a regular double-quoted string may span lines. With `false`, use triple quotes (`"""..."""`) for a multiline string. |
| `angelscript.engine.boolConversionMode` | `0` | With `1`, `if (obj)` can use a class’s `opImplConv` or `opConv` to `bool`. With `0`, use an explicit conversion. |
| `angelscript.engine.useCharacterLiterals` | `0` | With `0`, `'A'` is a string and assigning it to `int` raises a type error. With `1`, it is an integer character code. |
| `angelscript.engine.disallowValueAssignForRef` | `false` | With `true`, the editor flags value assignment (`a = b`) on reference types. Use handle assignment (`@a = @b`) where you mean to rebind the handle. |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | `name: value` works in all modes. For `name = value`, `0` reports an error, `1` reports a warning, and `2` accepts it. |
| `angelscript.engine.disableIntegerDivision` | `false` | With `false`, `1 / 2` has an integer type. With `true`, the editor treats division of two integers as a floating-point expression. |
| `angelscript.engine.disallowEmptyListElements` | `false` | With `true`, the editor flags the empty slot in `{1, , 3}`. |
| `angelscript.engine.foreachSupport` | `true` | With `false`, the editor flags `foreach` as unsupported. |
| `angelscript.engine.requireEnumScope` | `false` | With `true`, write `Color::Red`. The editor flags an unqualified `Red`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | With `true`, a class with `Item(int id)` also gets an implicit zero-argument constructor. With `false`, add `Item()` explicitly if you want to write `Item x;`. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | With `true`, the editor accepts non-ASCII letters in identifiers. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | With `true`, identical `shared interface` declarations can appear in multiple files without a duplicate declaration error. |
| `angelscript.engine.compilerWarnings` | `1` | `0` hides compiler warnings, `1` shows them as warnings, and `2` shows them as errors. |
| `angelscript.engine.noHandleTypes` | `[]` | List types that your host registers without handle support, such as `["string", "datetime"]`. The editor reports `as-err-handle-on-no-handle-type` (`E261`) for handles to the listed types. Entries are combined with `noHandleTypes` from `angelscript.rules.json`. |

### Preprocessor and Metadata Options

| Setting | Default | What changes in the editor |
| :--- | :--- | :--- |
| `angelscript.preprocessor.elseSupport` | `false` | With `true`, `#else` can become the active branch when `#if` is false. With `false`, a false `#if` still excludes lines through `#endif`. |
| `angelscript.preprocessor.elifSupport` | `false` | With `true`, the first matching branch in a chain with `#elif` is the one the editor analyzes. |
| `angelscript.preprocessor.ifdefSupport` | `false` | With `true`, `#ifdef` and `#ifndef` select branches using defined words from sources such as `angelscript.define` and stub `#define` lines. |
| `angelscript.preprocessor.defineInScripts` | `false` | With `true`, a `#define WORD` in an active part of a script can affect later `#if` checks. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | For `#pragma`, `"accept"` is silent, `"hint"` shows a hint, and `"error"` reports an error. |
| `angelscript.metadata.enabled` | `false` | With `true`, the editor accepts `[...]` metadata blocks before classes, interfaces, functions, and variables. |

## Formatting and `.clang-format`

Set brace placement and spacing with `angelscript.format.*`. When you format a file, the formatter looks in its directory and walks up through parent folders for `.clang-format`, `_clang-format`, or `.as-clang-format`. The supported options it finds override the corresponding editor settings.

You can use `BasedOnStyle`, `Language: AngelScript` or `Cpp` sections, `SpaceBeforeParens`, `PointerAlignment`, and `AllowShortBlocksOnASingleLine`. To leave a region alone, surround it with `// clang-format off` and `// clang-format on`.

## Path Variables

You can use these variables in settings that accept paths, such as `angelscript.searchDirectories`.

| Variable | Expands to |
| :--- | :--- |
| `${workspaceFolder}` | Root folder of the active workspace. |
| `${workspaceFolder:name}` | Root folder of a named workspace in a multi-root setup. |
| `${userHome}` | Current user's home folder. |
| `${env:NAME}` | Value of the environment variable `NAME`. |

## Contributed Commands

Open the Command Palette (`Ctrl+Shift+P` or `Cmd+Shift+P`). The Explorer also has commands for setting up modules and formatting stubs.

| Command | Title | What it does |
| :--- | :--- | :--- |
| `angelscript.configureHostEnvironment` | AngelScript: Configure Host Rules & Predefined API... | Opens the setup menu for stubs and engine rules. |
| `angelscript.ignoreUnusedSymbol` | AngelScript: Ignore Unused Symbol (Mark as Entry Point / Callback) | Adds the selected function or callback to the engine rules so it stops showing as unused. |
| `angelscript.selectPredefined` | AngelScript: Select Predefined Stub | Picks the active stub. |
| `angelscript.selectStubs` | AngelScript: Select Predefined Stubs | Picks several stubs to load together. |
| `angelscript.rescanWorkspace` | AngelScript: Rescan Workspace | Scans the workspace again for scripts and stubs. |
| `angelscript.statusMenu` | AngelScript: AngelScript Status Menu | Opens the menu for module and stub actions. |
| `angelscript.switchActiveModule` | AngelScript: Switch Active Module | Chooses which module is active. |
| `angelscript.showServerLog` | AngelScript: Show Server Log | Shows the language server output in VS Code. |
| `angelscript.openLogsFolder` | AngelScript: Open Logs Folder | Opens the folder that contains the server logs. |
| `angelscript.restartServer` | AngelScript: Restart Server | Restarts the language server. |
| `angelscript.setModuleEntryPoint` | Set as Module Entry Point | Right-click an `.as` file to make it a module entry point. |
| `angelscript.setModuleFolder` | Set as Module Folder | Right-click a folder to assign its scripts to a module. |
| `angelscript.formatPredefinedStub` | Format Predefined Stub | Right-click a `.predefined` file to format its declarations. |
| `angelscript.clearLocalState` | Clear Local Session Overrides | Clears the local module and stub choices and restores the workspace settings. |

## Debugger

Use `type: "angelscript"` in your VS Code debug configuration to set breakpoints and inspect the call stack and local variables. Choose `dap`, `asdbg`, or `cdebugger` with `angelscript.debug.defaultProtocol`. Set `angelscript.debug.defaultPort` to match your runtime. The default is `9002`.

## Building from Source

You’ll need a C++20 compiler and CMake 3.22 or later for the server. The client build uses Node.js and `npm`. Run these commands from the repository root.

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
| `angelscript.diagnostics.reportMissingClassAccessor` | `false` | Warn when a field or property of the enclosing class is accessed without `this.` inside a method (W123). Disabled by default. |
| `angelscript.diagnostics.reportUnusedMembers` | `false` | Warn when a class member variable or method is declared but never accessed or called in any script. |
| `angelscript.diagnostics.ignoredUnusedFunctions` | `[]` | Function names (short or qualified) exempt from unused-function warnings (e.g., `["main", "MapInit", "PluginInit"]`). |
| `angelscript.diagnostics.ignoredUnusedFunctionPatterns` | `[]` | Regular expression patterns exempting matching function names from unused-function warnings (e.g., `["^On.*", "^Hook_.*"]`). |
| `angelscript.diagnostics.ignoredUnusedPaths` | `[]` | Workspace-relative glob patterns that suppress unused declaration warnings W116–W120. `*` and `?` stay within one path segment, `**` crosses directories, and a trailing `/` selects descendants. Patterns without `/` match file names at any depth. Matching ignores case on Windows. |
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
| `angelscript.engine.noHandleTypes` | `[]` | Host value type names that do not support object handles (`@`), such as `string` or `datetime`.  Merged (set union) with the top-level `noHandleTypes` array in `angelscript.rules.json`. Declaring a handle to any listed type (e.g. `string@ s;` or `array<string@>`) reports `as-err-handle-on-no-handle-type` (`E261`). |
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

[README](../README.md) | [Engine Rules Guide](RULES.md) | [CHANGELOG.md](../CHANGELOG.md)
