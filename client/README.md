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
| **Sven Co-op** | [Sven Co-op - Gaftherman](https://github.com/Gaftherman/angelscript-lsp/blob/main/predefined/sven.as.predefined) | **Recommended (Recomendado)** — Actively maintained and updated for modern Sven Co-op 5.26+ engine API bindings, complete const-correctness, ref qualifiers, and math/engine structs. We strongly recommend this stub for all Sven Co-op scripting. |
| **Sven Co-op** | [Sven Co-op - Sashi0034](https://github.com/sashi0034/angel-lsp/blob/main/examples/Sven%20Co-op/as.predefined) | **Legacy (Heredado)** — Retained for backwards compatibility with older projects and configurations; outdated compared to modern engine releases. |
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

#### 1. General & Server Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.server.executablePath` | `""` | Custom path to the `angel_lsp` executable binary. Disabled in untrusted workspaces. |
| `angelscript.server.logLevel` | `"debug"` | Logging verbosity: `"error"`, `"warn"`, `"info"`, `"debug"`, or `"trace"`. |
| `angelscript.statusBar.enabled` | `true` | Controls whether the AngelScript status bar item is visible. |
| `angelscript.statusBar.alignment` | `"left"` | Alignment of the AngelScript status bar item (`"left"` or `"right"`). |
| `angelscript.statusBar.showStub` | `false` | Shows the active stub filename directly in the status bar label. |
| `angelscript.dimInactiveRegions` | `true` | Visually dims inactive preprocessor code blocks (`#if / #else / #endif`). |
| `angelscript.inactiveRegionOpacity` | `0.55` | Opacity of dimmed inactive preprocessor regions (between `0.1` and `1.0`). |

#### 2. Workspace & Script Modules Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.modules` | `[]` | Script module definitions specified as `{"name", "entry"}` or `{"name", "folder"}`. |
| `angelscript.moduleEntryPoint` | `""` | Single script entry point for the workspace if not using multi-module configuration. |
| `angelscript.searchDirectories` | `[]` | Extra directories to scan for `#include "path.as"` resolution. |
| `angelscript.include.implicitExtension` | `false` | Allows `#include "helper"` to resolve to `helper.as` without requiring the extension. |
| `angelscript.predefined.active` | `""` | The active stub to load when multiple are present. Set to `"all"` to merge all stubs. |
| `angelscript.predefinedFiles` | `[]` | Explicit list of predefined host API stub files (`.as.predefined`). |
| `angelscript.stubs.activeFiles` | `[]` | Multi-select list of active stub file paths enabled in the workspace. |
| `angelscript.forceIncludeFiles` | `[]` | List of header files automatically included into every script in the workspace. |
| `angelscript.exclude` | `["**/.git/**", "**/build/**", "**/node_modules/**"]` | Glob patterns excluded from workspace file scanning and indexing. |
| `angelscript.fileExtension` | `".as"` | Suffix identifying AngelScript script files in the workspace. |
| `angelscript.predefinedExtension` | `".as.predefined"` | Suffix identifying predefined host API stub files. |
| `angelscript.define` | `[]` | Global preprocessor symbol definitions (e.g. `["DEBUG", "CLIENT"]`). |
| `angelscript.arrayLikeTypes` | `[]` | Custom types treated as array-like containers for indexer inspection. |
| `angelscript.enableVirtualMixinDocuments` | `false` | Enables virtual document providers (`angelscript-virtual://`) for mixin class expansion. |

#### 3. Inlay Hints Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.features.inlayHints` | `true` | Master toggle for inlay hints support. |
| `angelscript.inlayHints.maxParameters` | `0` | Maximum number of parameter inlay hints displayed per call (`0` = unlimited). |
| `angelscript.inlayHints.maxLength` | `0` | Maximum character length for parameter hint labels before truncating (`0` = unlimited). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Suppresses parameter name hints when argument text matches parameter name. |
| `angelscript.inlayHints.enableTooltip` | `true` | Shows detailed documentation tooltips when hovering over parameter inlay hints. |
| `angelscript.inlayHints.enableLocation` | `true` | Enables `Ctrl+Click` navigation to the formal parameter definition from inlay hints. |
| `angelscript.inlayHints.omittedDefaultArguments` | `"off"` | Inlay hints for omitted default arguments: `"nameAndValue"`, `"declaration"`, or `"off"`. |

#### 4. Code Formatting Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.features.formatting` | `true` | Enables document formatting via the language server. |
| `angelscript.features.onTypeFormatting` | `false` | Enables formatting as you type (trigger characters: `;`, `}`). |
| `angelscript.format.onSave` | `false` | Automatically formats AngelScript documents when saving. |
| `angelscript.format.braceStyle` | `"allman"` | Brace placement style: `"allman"` (new line) or `"kr"` (same line). |
| `angelscript.format.spacesInsideParentheses` | `false` | Inserts spaces inside parentheses (e.g. `foo( bar )` instead of `foo(bar)`). |
| `angelscript.format.keepEmptyBlocksOnSingleLine` | `true` | Preserves empty blocks on a single line (e.g. `{}`) instead of expanding them. |

#### 5. Language Features & Autocompletion Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.features.hover` | `true` | Enables hover documentation tooltips. |
| `angelscript.hover.stringLiteralLength` | `true` | Displays character length and byte count on string literal hovers. |
| `angelscript.hover.stringLiteralPathResolution` | `true` | Resolves string literals that look like file/asset paths. |
| `angelscript.hover.assetSearchPaths` | `[]` | Additional search directories for resolving asset paths in string literals. |
| `angelscript.features.completion` | `true` | Enables intelligent code completion. |
| `angelscript.completion.smartTypeRanking` | `true` | Prioritizes completions matching expected parameter and assignment types. |
| `angelscript.completion.completeFunctionParens` | `true` | Automatically inserts parentheses and argument placeholders on function completion. |
| `angelscript.completion.qualifyEnumValues` | `true` | Suggests qualified enum values (`Enum::Value`) when enum scope is required. |
| `angelscript.features.definition` | `true` | Enables Go to Definition (`F12`). |
| `angelscript.features.references` | `true` | Enables Find All References (`Shift+F12`). |
| `angelscript.features.signatureHelp` | `true` | Enables parameter information tooltips while typing call arguments. |
| `angelscript.features.semanticTokens` | `true` | Enables semantic syntax highlighting tokens. |
| `angelscript.features.documentSymbols` | `true` | Enables outline view and breadcrumb symbols. |
| `angelscript.features.workspaceSymbols` | `true` | Enables workspace-wide symbol search (`Ctrl+T`). |
| `angelscript.features.rename` | `true` | Enables symbol renaming across the workspace (`F2`). |
| `angelscript.features.documentHighlight` | `true` | Highlights occurrences of the current symbol under the cursor. |
| `angelscript.features.foldingRange` | `true` | Enables code folding for blocks, classes, functions, and comments. |
| `angelscript.features.codeAction` | `true` | Enables Quick Fix code actions. |
| `angelscript.features.documentLink` | `true` | Detects clickable links in comments and string literal paths. |
| `angelscript.features.implementation` | `true` | Enables Go to Implementation for interfaces (`Ctrl+F12`). |
| `angelscript.features.selectionRange` | `true` | Enables smart expansion of selection ranges (`Shift+Alt+Right`). |
| `angelscript.features.callHierarchy` | `true` | Enables incoming and outgoing call tree indexing. |
| `angelscript.features.typeHierarchy` | `true` | Enables supertype and subtype class hierarchy navigation. |
| `angelscript.features.linkedEditing` | `true` | Synchronizes edits across matching identifiers. |
| `angelscript.features.codeLens` | `true` | Displays reference counts and accessor links above declarations. |
| `angelscript.features.pullDiagnostics` | `true` | Supports LSP pull diagnostic model. |
| `angelscript.features.typeConversionChecks` | `true` | Validates type conversions, implicit casts, and constructor arguments. |
| `angelscript.features.predefinedLoader` | `true` | Enables loading and indexing of predefined host stubs. |
| `angelscript.features.enableCommentSuppressions` | `true` | Recognizes inline `// @as-suppress` comments. |

#### 6. Semantic Diagnostics & Analysis Configuration

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.diagnosticSeverity` | `{}` | Per-code severity overrides (e.g. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.diagnostics.reportUnknownTypes` | `true` | Reports errors when encountering unresolved type identifiers. |
| `angelscript.diagnostics.reportAccessorPortability` | `true` | Warns about property accessor patterns that may not be portable across engines. |
| `angelscript.diagnostics.reportAccessorDisabled` | `true` | Warns when property accessors (`get_`/`set_`) are used while accessors are disabled. |
| `angelscript.diagnostics.reportBoolConversion` | `true` | Warns on unsafe implicit conversions to boolean. |
| `angelscript.diagnostics.reportMissingFuncdef` | `false` | Warns on missing or mismatched `funcdef` signatures. |
| `angelscript.diagnostics.reportIntegerDivision` | `false` | Warns about integer divisions that may silently truncate decimal parts. |
| `angelscript.diagnostics.reportPossibleNullDereference` | `true` | Warns about possible null handle dereferences (`as-warn-possible-null-dereference`). |
| `angelscript.diagnostics.reportHandleComparisonEquality` | `1` | Strictness for handle comparison: `0` (Disabled), `1` (Warning), `2` (Error). |
| `angelscript.diagnostics.missingAssetPathSeverity` | `"off"` | Severity for unresolved asset paths: `"off"`, `"hint"`, `"warning"`, or `"error"`. |

#### 7. Engine Dialect & Preprocessor Configuration (`asEP_*`)

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.engine.allowUnsafeReferences` | `false` | When true (`asEP_ALLOW_UNSAFE_REFERENCES`), permits unsafe references in function signatures. |
| `angelscript.engine.privatePropAsProtected` | `false` | Treats private class properties as protected (`asEP_PRIVATE_PROP_AS_PROTECTED`). |
| `angelscript.engine.disallowGlobalVars` | `false` | Disallows global variable declarations (`asEP_DISALLOW_GLOBAL_VARS`). |
| `angelscript.engine.propertyAccessorMode` | `2` | Property accessor mode: `2` (standard get/set), `3` (require accessor prefix). |
| `angelscript.engine.allowMultilineStrings` | `false` | Allows multi-line string literals without escaping (`asEP_ALLOW_MULTILINE_STRINGS`). |
| `angelscript.engine.boolConversionMode` | `0` | Boolean conversion mode: `0` (strict), `1` (allow numbers/handles). |
| `angelscript.engine.useCharacterLiterals` | `0` | Character literal interpretation: `0` (character code), `1` (single-char string). |
| `angelscript.engine.disallowValueAssignForRef` | `false` | Disallows value assignment for reference types (`asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE`). |
| `angelscript.engine.alterSyntaxNamedArgs` | `0` | Named arguments syntax style: `0` (disabled), `1` (`arg: val`), `2` (`arg = val`). |
| `angelscript.engine.disableIntegerDivision` | `false` | Disallows integer division operator (`asEP_DISABLE_INTEGER_DIVISION`). |
| `angelscript.engine.disallowEmptyListElements` | `false` | Disallows empty elements in initialization lists (`asEP_DISALLOW_EMPTY_LIST_ELEMENTS`). |
| `angelscript.engine.foreachSupport` | `true` | Enables support for `foreach` loops. |
| `angelscript.engine.requireEnumScope` | `false` | When true (`asEP_REQUIRE_ENUM_SCOPE`), enums must be qualified with `Enum::Member`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | When true (`asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`), synthesized default constructor is generated. |
| `angelscript.engine.allowUnicodeIdentifiers` | `false` | Allows unicode characters in identifier names (`asEP_ALLOW_UNICODE_IDENTIFIERS`). |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | When true (`asEP_IGNORE_DUPLICATE_SHARED_INTF`), identical shared interfaces are merged. |
| `angelscript.engine.compilerWarnings` | `1` | Compiler warning severity: `0` (silent), `1` (warnings), `2` (treat warnings as errors). |
| `angelscript.preprocessor.elseSupport` | `false` | Enables `#else` directive support in scripts. |
| `angelscript.preprocessor.elifSupport` | `false` | Enables `#elif` directive support in scripts. |
| `angelscript.preprocessor.ifdefSupport` | `false` | Enables `#ifdef` / `#ifndef` directive support in scripts. |
| `angelscript.preprocessor.defineInScripts` | `false` | Allows script files to define preprocessor symbols via `#define`. |
| `angelscript.preprocessor.pragmaMode` | `"accept"` | Pragma directive handling: `"accept"`, `"hint"`, or `"error"`. |

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
