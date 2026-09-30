# AngelScript Language Server (AngelLSP)

AngelLSP is a high-performance, thread-safe Language Server Protocol (LSP) server for the [AngelScript](https://www.angelcode.com/angelscript/) programming language (`.as`), built in native C++20 and powered by Tree-Sitter for concrete syntax tree parsing and semantic resolution.

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
- **Intelligent Hover & Completion**: Overload-isolated documentation tooltips at call sites, Doxygen docstring rendering (`@brief`, `@param`, `@return`), lambda contract resolution (`(anonymous function) -> FuncdefName`), and scope-aware member completions (`.`, `::`).
- **Engine Dialect & Host Integration**: Sven Co-op extensionless `#include` resolution, predefined host stubs (`.as.predefined`), and configurable preprocessor flags (`#if`, `#define`).
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

## Key Settings

| Setting | Default | Description |
| :--- | :--- | :--- |
| `angelscript.searchDirectories` | `[]` | Extra directories to scan for `#include` resolution. |
| `angelscript.include.implicitExtension` | `false` | Resolves `#include "helper"` to `helper.as` without requiring the file extension. |
| `angelscript.predefinedFiles` | `[]` | List of predefined host API stub files (`.as.predefined`). |
| `angelscript.predefined.active` | `""` | The active stub to load when multiple are present. Set to `"all"` to merge all stubs. |
| `angelscript.modules` | `[]` | Script compilation modules specified by entry file (`"entry"`) or directory (`"folder"`). |
| `angelscript.enableVirtualMixinDocuments` | `false` | Enables virtual document providers (`angelscript-virtual://`) for mixin class inspection. |
| `angelscript.inlayHints.maxParameters` | `0` | Maximum number of parameter inlay hints to display per call (`0` = unlimited). |
| `angelscript.inlayHints.maxLength` | `0` | Maximum character length for parameter inlay hint labels before truncating with `...` (`0` = unlimited). |
| `angelscript.inlayHints.suppressWhenArgumentMatchesName` | `false` | Suppresses parameter name hints when argument text matches parameter name. |
| `angelscript.format.braceStyle` | `"allman"` | Brace placement style (`"allman"` or `"kr"`). |
| `angelscript.format.spacesInsideParentheses` | `false` | Whether to insert spaces inside parentheses (e.g. `foo( bar )` instead of `foo(bar)`). |
| `angelscript.completion.smartTypeRanking` | `true` | Prioritizes autocompletions matching expected parameter or assignment target type. |
| `angelscript.diagnosticSeverity` | `{}` | Per-diagnostic severity overrides (e.g. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.engine.requireEnumScope` | `false` | When true (`asEP_REQUIRE_ENUM_SCOPE`), enums must be qualified with `Enum::Member`. |
| `angelscript.engine.alwaysImplDefaultConstruct` | `false` | When true (`asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT`), default constructor is always synthesized. |
| `angelscript.engine.ignoreDuplicateSharedIntf` | `false` | When true (`asEP_IGNORE_DUPLICATE_SHARED_INTF`), identical shared interfaces across files are ignored. |
| `angelscript.features.*` | `true` | Individual toggles for LSP features (hover, completion, formatting, etc.). |

---

## Acknowledgements & Credits

- **AngelScript Logo & Brand**: The official AngelScript icon is adapted from the [AngelScript website](https://www.angelcode.com/angelscript/) by Andreas Jönsson.
- **File Icons (`.as` / `.as.predefined`)**: Sourced from the wonderful [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (specifically the ActionScript icon), temporarily borrowed for testing while custom AngelScript icons are being designed—all credit and thanks to Philipp Kief and the Material Icon Theme contributors :P.

---

## License

This project is licensed under the MIT License. See the [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) file for details.
