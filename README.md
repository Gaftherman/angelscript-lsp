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
- **Intelligent Hover & Completion**: Overload-isolated documentation tooltips at call sites, Doxygen docstring rendering (`@brief`, `@param`, `@return`), lambda contract resolution (`(anonymous function) -> FuncdefName`), and scope-aware member completions (`.`, `->`, `::`).
- **Engine Dialect & Host Integration**: Sven Co-op extensionless `#include` resolution, predefined host stubs (`.as.predefined`), and configurable preprocessor flags (`#if`, `#define`).
- **High Performance & Low Overhead**: Native C++20, zero disk logging by default in release builds, zero-allocation token streams, and AST memory safety.
- **Native Bilingual Support**: Built-in dual localization for diagnostics, command titles, and configuration settings in English (`en`) and Spanish (`es`) via `@vscode/l10n`.

---

## Quickstart

### 1. Installation

Install via the Visual Studio Code Marketplace (search for `Angelscript`) or from a packaged `.vsix` bundle:
1. Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on macOS) and run `Extensions: Install from VSIX...`.
2. Select the compiled extension package (`angelscript.vsix` or `angelscript-0.8.5.vsix`).

### 2. Workspace Setup

Create or update `.vscode/settings.json` in your workspace folder:

```jsonc
{
  // Extra directories for #include resolution
  "angelscript.searchDirectories": [
    "${workspaceFolder}/scripts"
  ],

  // Load host engine API definitions
  "angelscript.predefinedFiles": [
    "${workspaceFolder}/stubs/sven.as.predefined"
  ],

  // Enable extensionless include resolution (e.g. #include "helper" finds "helper.as")
  "angelscript.include.implicitExtension": true
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
| `angelscript.modules` | `[]` | Script compilation modules specified by entry file or folder. |
| `angelscript.format.braceStyle` | `"allman"` | Brace placement style (`"allman"` or `"kr"`). |
| `angelscript.diagnosticSeverity` | `{}` | Per-diagnostic severity overrides (e.g. `{"as-warn-unused-variable": "hint"}`). |
| `angelscript.features.*` | `true` | Individual toggles for LSP features (hover, completion, formatting, etc.). |

---

## Acknowledgements & Credits

- **AngelScript Logo & Brand**: The official AngelScript icon is adapted from the [AngelScript website](https://www.angelcode.com/angelscript/) by Andreas Jönsson.
- **File Icons (`.as` / `.as.predefined`)**: Sourced from the wonderful [Material Icon Theme](https://github.com/PKief/vscode-material-icon-theme) (specifically the ActionScript icon), temporarily borrowed for testing while custom AngelScript icons are being designed—all credit and thanks to Philipp Kief and the Material Icon Theme contributors :P.

---

## License

This project is licensed under the MIT License. See the [LICENSE](https://github.com/Gaftherman/angelscript-lsp/blob/HEAD/LICENSE) file for details.
