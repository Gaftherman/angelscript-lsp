# Angelscript - Language Server for Angelscript (VS Code Extension)

**[English](README.md)** | **[Español](README.es.md)**

[![Visual Studio Marketplace Version](https://img.shields.io/visual-studio-marketplace/v/Gaftherman.angelscript-gaftherman.svg?label=Marketplace&color=blue)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![Visual Studio Marketplace Installs](https://img.shields.io/visual-studio-marketplace/i/Gaftherman.angelscript-gaftherman.svg?color=success)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Angelscript provides rich, high-performance language intelligence for the [AngelScript](https://www.angelcode.com/angelscript/) programming language (`.as`), powered by a native C++20 language server using Tree-Sitter for concrete syntax tree parsing, symbol analysis, and semantic resolution.

The entire workspace is analyzed directly from concrete syntax trees without intermediate disk dumps, script concatenation, or host engine execution. It is tailored for real-world AngelScript ecosystems (such as Sven Co-op, game engine hosts, and custom `CScriptBuilder` integrations), delivering sub-millisecond hover lookups, flow-sensitive null checks, type inference, and semantic navigation across the entire workspace.

---

## Quickstart

### 1. Installation

Install via the [Visual Studio Code Marketplace](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman) or search for `Angelscript` by Gaftherman in the Extensions view (`Ctrl+Shift+X`):
1. Open Visual Studio Code.
2. Press `Ctrl+P`, paste `ext install Gaftherman.angelscript-gaftherman`, and press Enter.

Alternatively, to install from a packaged `.vsix` bundle:
1. Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on macOS) and run `Extensions: Install from VSIX...`.
2. Select the compiled extension package (`angelscript.vsix` or `angelscript-*.vsix`).

### 2. Workspace Setup

Open your workspace folder in VS Code. Configure `.vscode/settings.json` according to your engine setup:

#### Example A: Sven Co-op (Folder Module)
Recommended for Sven Co-op map script folders (e.g. `maps/hcas`):
```jsonc
{
  // Allow extensionless includes (e.g. #include "helper" resolves to helper.as)
  "angelscript.include.implicitExtension": true,

  // Extra search root for relative and angular-bracket #include paths
  "angelscript.searchDirectories": [
    "${workspaceFolder}/maps"
  ],

  // Folder module ownership (all scripts under maps/hcas belong to module HCAS)
  "angelscript.modules": [
    {
      "name": "HCAS",
      "folder": "${workspaceFolder}/maps/hcas"
    }
  ],

  // Enable virtual mixin document inspection (angelscript-virtual://)
  "angelscript.enableVirtualMixinDocuments": true,

  // Active predefined host stub
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

#### Example B: Sven Co-op (Entry Script Module)
Recommended when compiling a script tree starting from a registration entry point:
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
  "angelscript.enableVirtualMixinDocuments": true,
  "angelscript.predefined.active": "${workspaceFolder}/maps/sven.as.predefined"
}
```

#### Example C: Generic Game Engine / Standalone Workspace
For standalone AngelScript host integrations using custom include paths and API stubs:
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

### 3. Predefined Host Stubs (`.as.predefined`)

In AngelScript, host applications register their C++ APIs (classes, global functions, properties, and constants) into the scripting engine at runtime. To provide accurate IntelliSense, autocompletion, type validation, and navigation for these host APIs, AngelLSP loads `.as.predefined` header stubs.

The following community and tested host stubs are available:

| Host Environment | Source & Link | Status & Recommendation |
| :--- | :--- | :--- |
| **Sven Co-op** | [Sven Co-op - Gaftherman](https://github.com/Gaftherman/angelscript-lsp/blob/main/predefined/sven.as.predefined) | **Recommended** — Actively maintained and updated for modern Sven Co-op 5.26+ engine API bindings, complete const-correctness, ref qualifiers, and math/engine structs. We strongly recommend this stub for all Sven Co-op scripting. |
| **Sven Co-op** | [Sven Co-op - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Sven%20Co-op/as.predefined) | **Legacy** — Retained for backwards compatibility with older projects and configurations; outdated compared to modern engine releases. |
| **Trackmania Nations Forever** | [Trackmania Nations Forever - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Trackmania%20Nations%20Forever/as.predefined) | Compatible — Host API bindings for Trackmania Nations Forever scripting (`CGameCtnApp`, `MwFastBuffer`, etc.). |
| **OpenSiv3D** | [OpenSiv3D - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/OpenSiv3D/as.predefined) | Compatible — Host API bindings for the OpenSiv3D C++ game framework (`Vec2`, `ColorF`, `Circle`, etc.). |

> [!TIP]
> To configure an active stub in your workspace, set `"angelscript.predefined.active": "${workspaceFolder}/path/to/stub.as.predefined"` or set `"all"` to merge multiple stubs. You can also click the status bar item or run `AngelScript: Select Active Host Stub` to switch stubs interactively.

---

## Contributed Commands

All commands can be triggered via the Command Palette (`Ctrl+Shift+P` / `Cmd+Shift+P`) or context menus:

| Command | Title | Context / Description |
| :--- | :--- | :--- |
| `angelscript.selectPredefined` | **AngelScript: Select Active Host Stub** | Opens a QuickPick list to switch the active `.as.predefined` host stub. |
| `angelscript.selectStubs` | **AngelScript: Select Host Stubs** | Opens a multi-select dialog to select and merge multiple host stubs. |
| `angelscript.rescanWorkspace` | **AngelScript: Rescan Workspace** | Forces a complete background rescan and re-indexing of all workspace files. |
| `angelscript.statusMenu` | **AngelScript: Status Menu** | Displays the server status menu, active stub, and quick actions. |
| `angelscript.showServerLog` | **AngelScript: Show Language Server Log** | Focuses the language server output channel in the Output panel. |
| `angelscript.openLogsFolder` | **AngelScript: Open Logs Folder** | Opens the directory containing local language server log files on disk. |
| `angelscript.restartServer` | **AngelScript: Restart Server** | Shuts down and restarts the language server process. |
| `angelscript.setModuleEntryPoint` | **AngelScript: Set as Module Entry Point** | Explorer context menu item on `.as` files to configure module entry point. |
| `angelscript.setModuleFolder` | **AngelScript: Set as Module Folder** | Explorer context menu item on folders to configure folder-based module ownership. |
| `angelscript.formatPredefinedStub` | **AngelScript: Format Predefined Stub Header** | Explorer context menu item on `.predefined` files to format host API headers. |
| `angelscript.viewMixinExpansion` | **AngelScript: View Mixin Expansion** | Opens the synthesized virtual document (`angelscript-virtual://`) for a mixin class. |
| `angelscript.peekMixinInline` | **AngelScript: Peek Mixin Inline** | Opens an inline peek view showing the expanded mixin implementation. |
| `angelscript.openPhysicalSource` | **AngelScript: Open Physical Source** | Navigates from a virtual mixin document back to the physical source code file. |

---

## Key Features & UI Highlights

- **Interactive Status Bar Item**: The "AngelScript IntelliSense" status item in the status bar displays server status and the active predefined stub name. Clicking it launches the interactive status menu or stub selector. Configurable alignment via `angelscript.statusBar.alignment` (`"left"` or `"right"`).
- **Inactive Preprocessor Region Dimming**: Blocks excluded by `#if / #else / #endif` preprocessor directives are automatically dimmed in the editor with configurable opacity (`angelscript.dimInactiveRegions` and `angelscript.inactiveRegionOpacity`).
- **Inlay Hints with Navigation**: Parameter name inlay hints display inline argument names. `Ctrl+Click` on any parameter hint navigates directly to the formal parameter definition. Supports omitted default argument hints (`nameAndValue`, `declaration`, `off`) and suppression when argument text matches parameter name.
- **Virtual Mixin Documents (`angelscript-virtual://`)**: Synthetic virtual document provider that expands AngelScript mixin classes within their target class scope for inline peek, navigation, and member validation.
- **Asset Path Probing**: String literals that resemble file or asset paths are probed against the workspace and asset directories (`angelscript.hover.assetSearchPaths`), displaying file existence, file size, and metrics on hover.
- **Flow-Sensitive Null Checks**: Intraprocedural diagnostics (`as-warn-possible-null-dereference`) warning on unchecked handles or handles used after null assignment.
- **Precise Hover & Navigation**: Overload-isolated documentation tooltips at call sites, Doxygen docstrings (`@brief`, `@param`, `@return`), constructor resolution, lambda signatures, Go to Definition (`F12`), and Go to Implementation (`Ctrl+F12`).
- **Autocompletion with Smart Type Ranking**: Contextual type ranking prioritizing matching parameter and assignment types, enum member qualification, and snippet expansion.
- **Native Clang-Format Engine**: Automatic discovery of `.clang-format`, `_clang-format`, and `.as-clang-format` with full LLVM Clang-Format style options (`BasedOnStyle` presets `LLVM`, `Google`, `Chromium`, `Mozilla`, `WebKit`, `Microsoft`, `GNU`, `Allman`, multi-language `Language: AngelScript` / `Cpp` sections, `BraceWrapping`, `SpaceBeforeParens`, `PointerAlignment`, `ShortBlocks/Functions/If/Loops`, `ReflowComments`, and `// clang-format off/on`).
- **Localized Quick Fixes (Code Actions)**: Quick fixes with dual English and Spanish localization for common compiler errors, unused variables, and diagnostic suppressions.

---

## Internationalization & Localization (i18n / l10n)

AngelLSP provides seamless, out-of-the-box bilingual localization in both **English** and **Spanish**:

- **Automatic Language Sync**: The extension automatically adapts to your VS Code display language (`Configure Display Language` in the Command Palette).
- **Extension UI & Settings**: All 95+ configuration settings, command titles, status bar items, and notification dialogs are natively localized via `@vscode/l10n` (`bundle.l10n.json` and `bundle.l10n.es.json`) and manifest NLS tables (`package.nls.json` and `package.nls.es.json`).
- **Server Diagnostics**: The language server forwards diagnostic messages in the active locale, ensuring compiler errors and hover descriptions match your language preference.
- **Manual Locale Override**: You can explicitly select your language by configuring the server startup argument or passing `--locale=es` / `--locale=en`.

---

## Configuration Reference

### Path Variables

Path-valued settings support dynamic variable expansions matching VS Code's `launch.json` standard:

| Variable | Expands To |
| :--- | :--- |
| `${workspaceFolder}` | The root directory of the active workspace folder. |
| `${workspaceFolder:name}` | The root directory of the named workspace folder in a multi-root workspace. |
| `${userHome}` | The current user's home directory. |
| `${env:NAME}` | Value of the environment variable `NAME` (e.g. `${env:SVENCOOP_DIR}`). |

---

### Settings Catalog

<!-- SETTINGS_CATALOG_START -->
#### 1. General & Server Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.server.executablePath` | `""` | Path to the AngelScript Language Server executable. |
| `angelscript.server.logLevel` | `"debug"` | Specifies the verbosity level for language server logging output. |
| `angelscript.statusBar.alignment` | `"left"` | Which side of the status bar the AngelScript item sits on. It is the way to reach the server log, a restart and the stub picker, so the left is the default: that is where the problem counts are, and where the eye already is when a diagnostic sends you looking. |
| `angelscript.statusBar.enabled` | `true` | Whether to display the AngelScript status bar item in the editor status bar. |
| `angelscript.statusBar.showStub` | `false` | Whether to display the active predefined stub name in the status bar item instead of only the language name. |
| `angelscript.dimInactiveRegions` | `true` | Dim the code inside a `#if` block the preprocessor drops, the way the C++ extension dims its inactive regions. A decoration rather than a colour: it dims whatever the syntax highlighting produced, brackets included, which a semantic token cannot do - the editor paints `(`, `{` and `[` from its own bracket-pair feature. |
| `angelscript.inactiveRegionOpacity` | `0.55` | How visible the dimmed code inside a dropped `#if` block stays. 1 is no dimming at all. Matches `#angelscript.dimInactiveRegions#`, which has to be on for this to do anything. |

#### 2. Workspace & Script Modules Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.modules` | `[]` | The script modules this workspace builds, as `{ "name": "...", "entry": "..." }` entries. A module is AngelScript's own unit of compilation, and the entry is the `.as` the host builds it from - everything that file reaches through `#include` belongs to it.  Empty by default, and empty changes nothing. It exists because `external shared class Foo;` compiles only when *another* module declares `shared class Foo` - measured, and with the definition in the same module the compiler still rejects it. Without knowing the modules the server cannot tell a correct declaration from a broken one, so it stays quiet; with them, it can say.  The `entry` accepts the same `${...}` variables as every other path setting. An entry may name a `folder` instead of, or as well as, an `entry` - a folder module owns every script under it, and the deepest folder wins when they nest. |
| `angelscript.moduleEntryPoint` | `""` | Global module entry-point script path. Restricts compilation closure to its forward dependency DAG. |
| `angelscript.searchDirectories` | `[]` | Custom directory search paths for resolving included script files (`#include "path.as"`).  Accepts the `${...}` variables `launch.json` uses: `${workspaceFolder}`, `${workspaceFolder:name}`, `${userHome}` and `${env:NAME}`. VS Code does not expand these in ordinary settings, so the extension does it - a variable this window cannot answer is left in the path as written and noted in the server log. |
| `angelscript.predefined.active` | `""` | Selects the single predefined stub loaded by the workspace scan.  When empty (the default) the scan loads the first stub it finds in path order and ignores the rest, and says which one it chose. Set it to `all` to load every discovered stub together, which is what a workspace with two stubs used to do by default - shared declarations then resolve more than once.  Does not affect `#angelscript.predefinedFiles`, which continue to load always.  The "AngelScript: Select Predefined Stub" command populates this setting for you.  Accepts the `${...}` variables `launch.json` uses: `${workspaceFolder}`, `${workspaceFolder:name}`, `${userHome}` and `${env:NAME}`. VS Code does not expand these in ordinary settings, so the extension does it - a variable this window cannot answer is left in the path as written and noted in the server log. |
| `angelscript.predefinedFiles` | `[]` | Predefined stub files declaring the host application's API, loaded by path. Absolute paths are used as-is, so the stub can live outside the workspace (e.g. `C:/Games/svencoop/as.predefined`); relative paths are resolved against each workspace folder. Files inside the workspace whose name ends with `#angelscript.predefinedExtension` are picked up automatically and need no entry here.  Accepts the `${...}` variables `launch.json` uses: `${workspaceFolder}`, `${workspaceFolder:name}`, `${userHome}` and `${env:NAME}`. VS Code does not expand these in ordinary settings, so the extension does it - a variable this window cannot answer is left in the path as written and noted in the server log. |
| `angelscript.stubs.activeFiles` | `[]` | List of active predefined stub files loaded into the workspace index. Managed dynamically via the AngelScript: Select Predefined Stubs command. |
| `angelscript.include.implicitExtension` | `false` | Let `#include "helper"` find `helper.as`. Off by default, which is AngelScript's own behaviour: CScriptBuilder opens exactly the text between the quotes and appends nothing, so `#include "helper"` looks for a file literally named `helper`. Turn it on for a host that resolves the name itself and requires the extension to be left off - Sven Co-op works this way, and there the short form is the correct spelling. The suffix appended is `angelscript.fileExtension`. |
| `angelscript.forceIncludeFiles` | `[]` | Files to force-include before analyzing any module or script. |
| `angelscript.exclude` | `["**/.git/**", "**/build/**", "**/node_modules/**"]` | Directory globs the workspace scans do not descend into. Supports `?`, `*` within a path segment and `**` across segments. Setting this replaces the defaults rather than adding to them. |
| `angelscript.fileExtension` | `".as"` | Filename suffix of AngelScript source files, used when scanning the workspace to build the `#include` graph. |
| `angelscript.predefinedExtension` | `".as.predefined"` | Filename suffix that marks a file found in the workspace as a predefined stub. This is a suffix, not a path - use `#angelscript.predefinedFiles` to load a specific file. |
| `angelscript.define` | `[]` | Words `#if` treats as defined, matching the words the host application passes to `CScriptBuilder::DefineWord`. A `#if WORD` whose word is not listed here is excluded from compilation, exactly as it is by the real preprocessor, and no diagnostics are reported inside it. A predefined stub can declare the same words with `#define WORD`, which is usually the better place for them: the stub already describes the host's engine setup and travels with it. |
| `angelscript.arrayLikeTypes` | `[]` | Names of template types whose initializer list is a plain repeat of their element type, the way `array<T>`'s is. The engine's default array type is always included.  This is shorthand. The general mechanism is a `/// @listpattern {repeat T}` tag on the class in your `.as.predefined` stub, copied from the type's own `asBEHAVE_LIST_FACTORY` registration — that also expresses patterns this setting cannot, such as `dictionary`'s `{repeat {string, ?}}`. Use this setting when you cannot edit the stub.  Either way it has to be stated rather than detected: `array<T>` and `optional<T>` are declared identically in a stub, and the compiler accepts `array<int> a = {1};` while rejecting `optional<int> o = {1};`. |
| `angelscript.enableVirtualMixinDocuments` | `false` | Enables experimental virtual text documents for mixin classes (angelscript-virtual://). When disabled, high-performance symbol synthesis is used. |

#### 3. Inlay Hints Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.features.inlayHints` | `true` | Show inline parameter-name and deduced-type hints. |
| `angelscript.inlayHints.maxParameters` | `0` | Maximum number of parameter inlay hints to display for a call. 0 means unlimited (show all parameters). |
| `angelscript.inlayHints.maxLength` | `0` | Maximum character length for parameter inlay hint labels before truncating with '...'. 0 means unlimited (never truncate). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Suppress parameter name hints when the argument expression text matches the parameter name exactly. Default is false. |
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
| `angelscript.features.pullDiagnostics` | `true` | Answer `textDocument/diagnostic` and `workspace/diagnostic` (LSP 3.17). The editor asks for diagnostics instead of waiting to be told. Push notifications are sent either way, so turning this off loses nothing a client that does not pull was using. |
| `angelscript.features.typeConversionChecks` | `true` | Report conversions with no constructor, `opConv`/`opImplConv` or `opCast`/`opImplCast` to back them. |
| `angelscript.features.predefinedLoader` | `true` | Load predefined stub files describing the host application's API. |
| `angelscript.features.enableCommentSuppressions` | `true` | Enable comment-based diagnostic suppressions using '// disable <CODE>' and '// enable <CODE>' (e.g. '// disable W156'). |

#### 6. Semantic Diagnostics & Analysis Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.diagnosticSeverity` | `{}` | Overrides the severity of individual diagnostics, keyed by diagnostic code. Example: `{ "as-warn-unused-variable": "hint" }`. |
| `angelscript.diagnostics.reportUnknownTypes` | `true` | Report a parameter or return type that resolves to no declaration.  `void f(TypoTypeName x)` is a compile error — the engine answers "Identifier 'TypoTypeName' is not a data type in namespace 'TEST' or parent" — and left unreported it surfaces as silence at every call site instead, because a call whose parameter types are unknown cannot be checked either.  It is also, from the server's side, indistinguishable from a legitimate engine-registered type. Turn it off if your host registers types in C++ and declares none of them — or better, provide a predefined stub file via `#angelscript.predefinedFiles`. |
| `angelscript.diagnostics.reportAccessorPortability` | `true` | Hint on a `get_`/`set_` accessor written without the `property` keyword. Such an accessor is a property under `asEP_PROPERTY_ACCESSOR_MODE` 2, this server's default, but not under mode 3, which is the engine's own. Adding the keyword is accepted under both, so the quick fix cannot break a working build - which is why this one is on by default. |
| `angelscript.diagnostics.reportAccessorDisabled` | `true` | Hint where a script property accessor is used as a property but the host disabled those (`angelscript.engine.propertyAccessorMode` 0 or 1).  Under either mode the compiler skips script-defined accessors entirely, so `c.X` backed by a script `get_X`/`set_X` is rejected — with the `property` keyword and without it. A hint rather than an error, and off by default, because the analyzer is being told what the host does: a host told wrong would otherwise see errors on code that builds for it. Says nothing under modes 2 and 3. |
| `angelscript.diagnostics.reportBoolConversion` | `true` | Hint when a class is used where a `bool` is expected, such as `if (h)`. Under `asEP_BOOL_CONVERSION_MODE` 0, the engine's default, this is a compile error even when the class declares `opImplConv`. A quick fix calls the conversion operator explicitly, which compiles under both modes. Off by default, and silent entirely when `angelscript.engine.boolConversionMode` is 1. |
| `angelscript.diagnostics.reportMissingFuncdef` | `false` | Hint when a type position names a function rather than a type, such as `Foo@ h` where `Foo` is a function. A function handle needs a `funcdef` to name its signature, and a quick fix declares one from the function's own parameters and return type. Off by default, since the name could belong to a host type this analyzer cannot see. |
| `angelscript.diagnostics.reportIntegerDivision` | `false` | Hint on `1 / 2`, which truncates to 0 under the engine's default. Off by default because a codebase that means integer division writes exactly this. |
| `angelscript.diagnostics.reportPossibleNullDereference` | `true` | Warn when a handle parameter, handle cast result, or uninitialized handle is dereferenced without a preceding null check (`!is null`, early guard return, etc.). Enabled by default. |
| `angelscript.diagnostics.reportHandleComparisonEquality` | `1` | Configure severity for handle equality comparisons with null (`== null`, `!= null`) instead of handle identity (`is null`, `!is null`). 0: disabled, 1: warning (compiler default), 2: error. |
| `angelscript.diagnostics.missingAssetPathSeverity` | `"off"` | Diagnostic severity for string literals referencing missing asset files. Off by default. |

#### 7. Engine Dialect & Preprocessor Configuration (asEP_*)

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | Set this if the host calls `SetEngineProperty(asEP_ALLOW_UNSAFE_REFERENCES, true)`.  With it off — the engine's default — `&` on a parameter means `&inout` and only an object type that supports handles may use it, so `void f(int &x)` is an error. With it on, primitives may be passed by reference and the diagnostic is not reported. |
| `angelscript.engine.privatePropAsProtected` | `false` | Set this if the host calls `SetEngineProperty(asEP_PRIVATE_PROP_AS_PROTECTED, true)`.  A `private` member then follows the `protected` rule, so a derived class may reach it and the access is no longer reported. |
| `angelscript.engine.disallowGlobalVars` | `false` | Set this if the host calls `SetEngineProperty(asEP_DISALLOW_GLOBAL_VARS, true)`.  Every global variable declaration is then a compile error — the engine answers "Global variables have been disabled by the application" — and is reported as one. |
| `angelscript.engine.propertyAccessorMode` | `2` | How `get_X()` / `set_X(v)` become the virtual property `X`, matching the host's `SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE, ...)`.  `2` — any accessor counts. `3` — only one carrying the `property` keyword does, and `c.X` without it is an error the engine reports as "'X' is not a member of 'C'".  The engine's own default is `3`; this server's is `2`, because being lenient here misses an error while being strict invents one for every host that sets `2`. Set it to `3` if yours does not. |
| `angelscript.engine.allowMultilineStrings` | `false` | Whether a plain `"..."` string may span lines, matching the host's `SetEngineProperty(asEP_ALLOW_MULTILINE_STRINGS, ...)`.  Off in the engine and off here: such a string is rejected with "Multiline strings are not allowed in this application". A `"""heredoc"""` spans lines under either setting. Turn this on only if your host does, or the server will report code your engine accepts. |
| `angelscript.engine.boolConversionMode` | `0` | How a class may be used where a `bool` is expected, matching the host's `SetEngineProperty(asEP_BOOL_CONVERSION_MODE, ...)`.  `0` — never; `if (h)` on a class is a compile error even when the class declares `opImplConv`. `1` — a class declaring `opImplConv` or `opConv` may be used as a condition.  `0` is the engine's own default and this server's. Set it to `1` if your host sets it, or the accompanying hint will describe a restriction you do not have. |
| `angelscript.engine.useCharacterLiterals` | `0` | How `'x'` is read, matching asEP_USE_CHARACTER_LITERALS. `0` — a one-character string, the engine's default, so `int c = 'x'` is a compile error. `1` — an integer character code, so the same line compiles. |
| `angelscript.engine.disallowValueAssignForRef` | `false` | asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE. When the host sets it, `a = b` on a reference type is an error and `@a = @b` is required. |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | asEP_ALTER_SYNTAX_NAMED_ARGS. `0` — only `name: value`, the engine's default, and `name = value` is an error. `1` — `name = value` accepted with a warning. `2` — accepted silently. |
| `angelscript.engine.disableIntegerDivision` | `false` | asEP_DISABLE_INTEGER_DIVISION. When the host sets it, `/` on two integers yields a float, so `1 / 2` is 0.5 rather than 0. |
| `angelscript.engine.disallowEmptyListElements` | `false` | asEP_DISALLOW_EMPTY_LIST_ELEMENTS. When the host sets it, a hole in an initializer list such as `{1, , 3}` is an error. |
| `angelscript.engine.foreachSupport` | `true` | asEP_FOREACH_SUPPORT. ON by the engine's own default; turn it off only if your host disables `foreach`, otherwise every `foreach` loop is reported. |
| `angelscript.engine.requireEnumScope` | `false` | asEP_REQUIRE_ENUM_SCOPE. When turned on, an unqualified enumerator stops resolving and must be scoped with its enum type name, otherwise 'No matching symbol' is reported. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT. ON by the engine's default; when turned off, a class that declares only a non-default constructor can no longer be default-constructed. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | asEP_ALLOW_UNICODE_IDENTIFIERS. When turned on, non-ASCII Unicode characters are accepted in identifiers instead of producing parse errors. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | asEP_IGNORE_DUPLICATE_SHARED_INTF. When turned on, declaring the same shared interface more than once compiles cleanly instead of reporting a name conflict. |
| `angelscript.engine.compilerWarnings` | `1` | asEP_COMPILER_WARNINGS. Controls the severity of compiler warnings: 0 suppresses warnings entirely, 1 emits them as warnings (engine default), and 2 turns them into compile errors. |
| `angelscript.preprocessor.elseSupport` | `false` | Set this if the host patched its copy of `scriptbuilder.cpp` to understand `#else`.  The add-on shipped with the SDK does not. Measured against the real compiler: in `#if FOO / a / #else / b / #endif` with `FOO` undefined, the `#else` branch does **not** become the taken one — it is blanked along with the rest of the block, because `#else` is not a directive and the exclusion runs to the `#endif` regardless. With this off, that is exactly what this server assumes. |
| `angelscript.preprocessor.elifSupport` | `false` | Set this if the host patched its copy of `scriptbuilder.cpp` to understand `#elif`.  Not in the stock add-on. With it on, the first branch whose word is defined is the live one and every other branch is excluded. |
| `angelscript.preprocessor.ifdefSupport` | `false` | Set this if the host patched its copy of `scriptbuilder.cpp` to understand `#ifdef` and `#ifndef`.  Not in the stock add-on, where either one is left in the source and the compiler reports `Unexpected token`. |
| `angelscript.preprocessor.defineInScripts` | `false` | Set this if the host patched its copy of `scriptbuilder.cpp` so `#define WORD` in a script defines a word.  Not in the stock add-on, where `DefineWord` is a C++ call the host makes and a `#define` written in a script is a syntax error. To declare the words the host itself defines, use `#angelscript.define` or a `#define` line in a predefined stub, which are not affected by this setting. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | What to report for a `#pragma`.  The stock add-on rejects every one: with no pragma callback registered it substitutes a failure for the callback's answer, writes `Invalid #pragma directive` and fails the whole section. The default here is nevertheless `accept`, because a host that registers a callback is the common case and reporting an error by default would put a squiggle on a pragma that builds fine. Choose `error` for a host that really registered nothing, or `hint` if you are not sure. |
<!-- SETTINGS_CATALOG_END -->
---

## Supported File Types & TextMate Grammars

AngelLSP registers dedicated language identifiers, syntax highlighting grammars, and snippet libraries for:

- **AngelScript Scripts (`.as`)**: Language ID `angelscript`. Standard AngelScript script files with syntax highlighting, indentation rules, bracket matching, and code snippets.
- **Predefined Host Stubs (`.as.predefined`, `.predefined`)**: Language ID `angelscript-predefined`. Header stub definitions exposing host C++ API declarations to the language server.

---

## Workspace Trust & Security

AngelLSP implements strict security boundaries under VS Code's Workspace Trust model:
- In **Untrusted Workspaces**, custom server executable paths configured in workspace settings (`server.executablePath`) are strictly disabled and ignored.
- Only the bundled language server binary or global user settings may be used, protecting against remote code execution via untrusted repository configuration.

---

## Acknowledgements & Credits

- **AngelScript Logo & Brand**: The official AngelScript icon is adapted from the [AngelScript website](https://www.angelcode.com/angelscript/) by Andreas Jönsson.
- **File Icons (`.as` / `.as.predefined`)**: Sourced from the wonderful [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (specifically the ActionScript icon), temporarily borrowed for testing while custom AngelScript icons are being designed—all credit and thanks to Philipp Kief and the Material Icon Theme contributors :P.

---

## License

This project is licensed under the MIT License. See the [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) file for details.
