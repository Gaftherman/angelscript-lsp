# AngelScript Language Server (AngelLSP)

**[English](README.md)** | **[Español](README.es.md)**

[![Visual Studio Marketplace Version](https://img.shields.io/visual-studio-marketplace/v/Gaftherman.angelscript-gaftherman.svg?label=Marketplace&color=blue)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![Visual Studio Marketplace Installs](https://img.shields.io/visual-studio-marketplace/i/Gaftherman.angelscript-gaftherman.svg?color=success)](https://marketplace.visualstudio.com/items?itemName=Gaftherman.angelscript-gaftherman)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

AngelLSP is a high-performance, thread-safe Language Server Protocol (LSP) server for the [AngelScript](https://www.angelcode.com/angelscript/) programming language (`.as`), built in native C++20 and powered by Tree-Sitter for concrete syntax tree parsing, symbol analysis, and semantic resolution.

Unlike approaches that rely on running scripts inside an embedded host runtime or crude source-text concatenation, AngelLSP analyzes source files, module entry points, and predefined host stubs directly from abstract syntax trees. It is specifically tailored for real-world AngelScript ecosystems (such as Sven Co-op, game engine script hosts, and custom `CScriptBuilder` integrations), delivering sub-millisecond hover lookups, flow-sensitive null checks, type inference, and semantic navigation across the entire workspace.

> [!WARNING]
> ### Project Status: Work in Progress (WIP)
> **AngelLSP is currently under active development and experimental validation.** While it already provides rich language intelligence and continuous parity verification against the reference compiler, certain language constructs and edge cases are still evolving.
>
> **Recommended Alternative for Production:**  
> If you need a battle-tested, mature language server for daily production work or mission-critical AngelScript projects right now, we **strongly recommend** using [**sashi0034/angel-lsp**](https://github.com/sashi0034/angel-lsp).

---

## Core Capabilities

- **Flow-Sensitive Diagnostics**: Intraprocedural null handle dereference checks (`as-warn-possible-null-dereference`), syntax error recovery, and compiler parity validation against the reference compiler.
- **Precision Navigation**: Overload-aware Go to Definition, Declaration, Type Definition, Implementation (`Ctrl+F12`), and bi-directional Call & Type Hierarchies.
- **Intelligent Hover & Completion**: Overload-isolated documentation tooltips at call sites, Doxygen docstring rendering (`@brief`, `@param`, `@return`), constructor resolution, lambda contract resolution (`(anonymous function) -> FuncdefName`), and scope-aware member completions (`.`, `::`).
- **Interactive UI & Inactive Region Dimming**: Status bar item ("AngelScript IntelliSense") with active stub switcher, automatic dimming of inactive preprocessor code blocks (`#if / #else / #endif`), and parameter inlay hints with `Ctrl+Click` navigation.
- **Virtual Mixin Documents**: Synthetic document inspection (`angelscript-virtual://`) enabling inline peek and host-scoped member validation.
- **Asset Path Probing**: String literals matching asset or script file paths are probed against the workspace and asset directories for existence and metrics on hover.
- **Engine Dialect & Host Integration**: Sven Co-op extensionless `#include` resolution, predefined host stubs (`.as.predefined`), and configurable preprocessor flags (`#if`, `#define`).
- **Native Clang-Format Engine**: Full LLVM [Clang-Format Style Options](https://clang.llvm.org/docs/ClangFormatStyleOptions.html) support via `.clang-format`, `_clang-format`, or `.as-clang-format` (including `BasedOnStyle` presets `LLVM`, `Google`, `Chromium`, `Mozilla`, `WebKit`, `Microsoft`, `GNU`, `Allman`, multi-language `Language: AngelScript` / `Cpp` sections, `BraceWrapping`, `SpaceBeforeParens`, `PointerAlignment`, `ShortBlocks/Functions/If/Loops`, `ReflowComments`, and `// clang-format off/on`).
- **High Performance & Low Overhead**: Native C++20, zero disk logging by default in release builds, zero-allocation token streams, and AST memory safety.
- **Native Bilingual Support**: Built-in dual localization for diagnostics, command titles, and configuration settings in English (`en`) and Spanish (`es`) via `@vscode/l10n`.

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

Create or update `.vscode/settings.json` in your workspace folder depending on your engine setup:

#### Example A: Sven Co-op (Folder Module)
Recommended for Sven Co-op multi-script packages (e.g. `maps/hcas`):
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
Recommended when a script tree is compiled starting from a specific registration file:
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

#### Example C: Generic Game Engine / Standalone Project
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
> To configure an active stub in your workspace, set `"angelscript.predefined.active": "${workspaceFolder}/path/to/stub.as.predefined"` or set `"all"` to merge multiple stubs. All stubs are benchmarked and verified for fast, error-free parsing.

---

## Contributed Commands

All commands can be invoked from the Command Palette (`Ctrl+Shift+P` / `Cmd+Shift+P`) or file context menus:

| Command | Title | Description |
| :--- | :--- | :--- |
| `angelscript.selectPredefined` | **AngelScript: Select Active Host Stub** | Switches the active `.as.predefined` host stub via a QuickPick list. |
| `angelscript.selectStubs` | **AngelScript: Select Host Stubs** | Multi-select dialog to select and merge multiple host stubs. |
| `angelscript.rescanWorkspace` | **AngelScript: Rescan Workspace** | Forces a complete background re-indexing of all workspace files. |
| `angelscript.statusMenu` | **AngelScript: Status Menu** | Displays the server status menu, active stub, and quick actions. |
| `angelscript.showServerLog` | **AngelScript: Show Language Server Log** | Focuses the language server output channel in the Output panel. |
| `angelscript.openLogsFolder` | **AngelScript: Open Logs Folder** | Opens the directory containing local language server log files. |
| `angelscript.restartServer` | **AngelScript: Restart Server** | Shuts down and restarts the language server process. |
| `angelscript.setModuleEntryPoint` | **AngelScript: Set as Module Entry Point** | Context menu action on `.as` files to configure module entry point. |
| `angelscript.setModuleFolder` | **AngelScript: Set as Module Folder** | Context menu action on folders to configure folder module ownership. |
| `angelscript.formatPredefinedStub` | **AngelScript: Format Predefined Stub Header** | Context menu action on `.predefined` files to format API headers. |
| `angelscript.viewMixinExpansion` | **AngelScript: View Mixin Expansion** | Opens the synthesized virtual document (`angelscript-virtual://`). |
| `angelscript.peekMixinInline` | **AngelScript: Peek Mixin Inline** | Opens an inline peek view showing the expanded mixin implementation. |
| `angelscript.openPhysicalSource` | **AngelScript: Open Physical Source** | Navigates from a virtual mixin document to the physical source file. |

---

## Building from Source

### Prerequisites
- C++20 compiler: MSVC 2022 (v143) on Windows, GCC 13+ or Clang 16+ on Linux/macOS.
- CMake 3.22+ and Ninja (recommended).
- Node.js 18+ and `npm` (for the VS Code client).

### Build Commands

```bash
# 1. Build the C++20 language server
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release

# 2. Run test suite
ctest --test-dir server/build -C Release --output-on-failure

# 3. Build the VS Code extension client
cd client && npm install && npm run compile
```

---

## Architectural Layers & Include Matrix

Layer isolation is strictly enforced by `server/scripts/check-layer-includes.py`:

| Layer | Path | Allowed to `#include` | Strictly FORBIDDEN to `#include` |
| :--- | :--- | :--- | :--- |
| **Layer 1: Core / Config** | `core/`, `config/`, `document/`, `parser/`, `utils/` | Own layer, standard C++ libraries | Layers 2, 3, and 4 |
| **Layer 2: Analysis** | `analysis/` | Layer 1, standard C++ libraries | Layers 3 and 4 |
| **Layer 3: Features** | `features/<feature>/` | Layers 1 and 2 | Sibling features, Layer 4 |
| **Layer 4: Server / LSP** | `lsp/`, `main.cpp` | Layers 1, 2, and 3 | None (topmost layer) |

---

## Configuration Reference

### Path Variables

| Variable | Expands To |
| :--- | :--- |
| `${workspaceFolder}` | The root directory of the active workspace folder. |
| `${workspaceFolder:name}` | The root directory of the named workspace folder in a multi-root workspace. |
| `${userHome}` | The current user's home directory. |
| `${env:NAME}` | Value of the environment variable `NAME` (e.g. `${env:SVENCOOP_DIR}`). |

### Key Settings

#### 1. General & Server Configuration
| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.server.executablePath` | `""` | Custom path to the `angel_lsp` executable binary. Disabled in untrusted workspaces. |
| `angelscript.server.logLevel` | `"debug"` | Logging verbosity: `"error"`, `"warn"`, `"info"`, `"debug"`, or `"trace"`. |
| `angelscript.statusBar.enabled` | `true` | Controls whether the AngelScript status bar item is visible. |
| `angelscript.statusBar.alignment` | `"left"` | Alignment of the AngelScript status bar item (`"left"` or `"right"`). |
| `angelscript.dimInactiveRegions` | `true` | Visually dims inactive preprocessor code blocks (`#if / #else / #endif`). |
| `angelscript.inactiveRegionOpacity` | `0.55` | Opacity of dimmed inactive preprocessor regions (between `0.1` and `1.0`). |

#### 2. Workspace & Modules
| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.modules` | `[]` | Script module definitions specified as `{"name", "entry"}` or `{"name", "folder"}`. |
| `angelscript.searchDirectories` | `[]` | Extra directories to scan for `#include "path.as"` resolution. |
| `angelscript.include.implicitExtension` | `false` | Allows `#include "helper"` to resolve to `helper.as` without requiring the extension. |
| `angelscript.predefined.active` | `""` | The active stub to load when multiple are present. Set to `"all"` to merge all stubs. |
| `angelscript.predefinedFiles` | `[]` | Explicit list of predefined host API stub files (`.as.predefined`). |
| `angelscript.exclude` | `["**/.git/**", "**/build/**", "**/node_modules/**"]` | Glob patterns excluded from workspace scanning. |
| `angelscript.enableVirtualMixinDocuments` | `false` | Enables virtual document providers (`angelscript-virtual://`) for mixins. |

#### 3. Inlay Hints
| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.features.inlayHints` | `true` | Master toggle for parameter inlay hints. |
| `angelscript.inlayHints.maxParameters` | `0` | Maximum number of parameter inlay hints displayed per call (`0` = unlimited). |
| `angelscript.inlayHints.maxLength` | `0` | Maximum character length for parameter hint labels before truncating (`0` = unlimited). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Suppresses parameter name hints when argument text matches parameter name. |
| `angelscript.inlayHints.omittedDefaultArguments` | `"off"` | Inlay hints for omitted default arguments: `"nameAndValue"`, `"declaration"`, `"off"`. |

#### 4. Formatting
| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.format.braceStyle` | `"allman"` | Brace placement style: `"allman"` (new line) or `"kr"` (same line). |
| `angelscript.format.spacesInsideParentheses` | `false` | Inserts spaces inside parentheses (e.g. `foo( bar )` instead of `foo(bar)`). |
| `angelscript.format.keepEmptyBlocksOnSingleLine` | `true` | Preserves empty blocks on a single line (e.g. `{}`). |
| `angelscript.format.pointerAlignment` | `"left"` | Handle (`@`) and reference (`&`) alignment: `"left"` (`Foo@ bar`), `"right"` (`Foo @bar`), or `"middle"` (`Foo @ bar`). |

> **Tip:** Placing a `.clang-format`, `_clang-format`, or `.as-clang-format` file in your workspace or parent directory automatically overrides editor settings with full Clang-Format YAML options (`BasedOnStyle`, `BraceWrapping`, `PointerAlignment`, `ColumnLimit`, `IndentCaseLabels`, `SortIncludes`, etc.).

#### 5. Diagnostics & Engine Dialect
| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.diagnosticSeverity` | `{}` | Per-code severity overrides (e.g. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.diagnostics.reportPossibleNullDereference` | `true` | Intraprocedural null dereference checks. |
| `angelscript.engine.requireEnumScope` | `false` | When true (`asEP_REQUIRE_ENUM_SCOPE`), enums must be qualified with `Enum::Member`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | When true (`asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`), synthesized default constructor is generated. |
| `angelscript.engine.allowUnsafeReferences` | `false` | When true (`asEP_ALLOW_UNSAFE_REFERENCES`), permits unsafe references in signatures. |
| `angelscript.engine.propertyAccessorMode` | `2` | Property accessor mode: `2` (standard get/set), `3` (require accessor prefix). |
| `angelscript.engine.allowMultilineStrings` | `false` | Allows multi-line strings without escaping (`asEP_ALLOW_MULTILINE_STRINGS`). |
| `angelscript.engine.disableIntegerDivision` | `false` | Disallows integer division operator (`asEP_DISABLE_INTEGER_DIVISION`). |

---

## Acknowledgements & Credits

- **AngelScript Logo & Brand**: The official AngelScript icon is adapted from the [AngelScript website](https://www.angelcode.com/angelscript/) by Andreas Jönsson.
- **File Icons (`.as` / `.as.predefined`)**: Sourced from the wonderful [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (specifically the ActionScript icon), temporarily borrowed for testing while custom AngelScript icons are being designed—all credit and thanks to Philipp Kief and the Material Icon Theme contributors :P.

---

## License

This project is licensed under the MIT License. See the [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) file for details.
