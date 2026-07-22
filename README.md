# AngelScript Language Server (AngelLSP)

AngelLSP is a high-performance, thread-safe Language Server Protocol (LSP) implementation for the [AngelScript](https://www.angelcode.com/angelscript/) programming language (`.as` files). Built with C++20, it features a 100% pure **Tree-Sitter** & **SymbolTable** analysis architecture for instant response times and low memory footprint, paired with a Visual Studio Code extension client.

---

## Features

- **Pure Tree-Sitter Analysis Engine**: Complete AST parsing without native C++ engine binding callbacks or physical script concatenation.
- **Dual-Pass Diagnostics (`ValidationOracle`)**: 
  - **Syntax Pass**: Instant syntax error detection (`TSNode` error/missing node catching).
  - **Semantic Pass**: Workspace and document-level symbol resolution diagnostics.
- **Hover Information (`textDocument/hover`)**: Rich Markdown tooltips displaying function signatures, variable types, class properties, and parsed Doxygen documentation.
- **Go to Definition & Type Definition (`textDocument/definition`, `textDocument/typeDefinition`)**: Precise symbol lookup across documents, namespaces, classes, and global scopes.
- **Auto-Completion (`textDocument/completion`)**: Context-aware completion suggestions for global symbols, class member functions/properties, and namespace scopes.
- **Semantic Tokens (`textDocument/semanticTokens/full`)**: Full semantic syntax highlighting for keywords, types, functions, variables, parameters, and enum members.
- **Signature Help (`textDocument/signatureHelp`)**: Active parameter highlight and signature preview for function calls.
- **Workspace Predefined Loader (`as.predefined`)**: Native Tree-Sitter parsing of host application declarations (`as.predefined` or `.as` files).
- **Diagnostic Localization (`i18n`)**: Multi-language diagnostic error reporting supporting English (`en-US`) and Spanish (`es-ES`).
- **Protected JSON-RPC Stream**: Internal server logging routes strictly to `stderr` (`spdlog::stderr_color_mt`) and `window/logMessage` notifications, ensuring `stdout` is 100% clean for VS Code JSON-RPC streams.

---

## High-Performance C++20 Architecture

AngelLSP follows a 4-layer unidirectional architecture designed to eliminate circular dependencies, race conditions, and architectural leakages:

1. **Layer 4: LSP Orchestrator & Server (`lsp/`, `main.cpp`)**: Manages JSON-RPC message dispatching, client capability announcement, and configuration state.
2. **Layer 3: Feature Handlers (`features/`)**: Decoupled, stateless pure functions handling specific LSP requests (`hover`, `definition`, `completion`, `semantic_tokens`, `signature_help`).
3. **Layer 2: Analysis & Symbol Management (`analysis/`)**: Manages global and local symbol tables (`SymbolTable`), Tree-Sitter AST symbol extraction (`SymbolCollector`), scope resolution (`SymbolResolver`), and diagnostic caching (`DiagnosticCache`).
4. **Layer 1: Core, Document, Parser & Utilities (`document/`, `parser/`, `utils/`, `config/`, `i18n/`)**: Thread-safe document AST container (`Document`), Tree-Sitter parser queries, Doxygen docstring extractor, and configuration options.

### Performance Optimizations

- **Zero-Allocation String Lookups (`std::string_view`)**: Used across all read-only symbol queries and parser validation checks to minimize heap allocations.
- **High-Performance Hashing (`ankerl::unordered_dense`)**: Utilizes `ankerl::unordered_dense::map` and `ankerl::unordered_dense::set` for flat, cache-friendly, ultra-fast symbol lookups.
- **Modern String Formatting (`fmt::format`)**: Uses bundled `fmt` formatting for zero-overhead string construction.
- **Concurrent Read Safety**: Handlers execute concurrent read-only queries against symbol tables using `std::shared_mutex` (`std::shared_lock`).
- **Asynchronous Validation Worker**: Document validation runs on a dedicated background worker (`std::jthread`) with a 300ms debounce timer.

---

## Building from Source

### Prerequisites

- **C++ Compiler**: C++20 compliant compiler:
  - Windows: Visual Studio 2022 (MSVC v143) with MASM support.
  - Linux: GCC 13+ or Clang 16+.
- **Build System**: CMake 3.22 or newer.
- **Client Prerequisites**: Node.js v18+ and `npm`.

### Build Instructions

#### Windows (PowerShell / Command Prompt)

```powershell
# 1. Clone repository
git clone https://github.com/Gaftherman/angelscript-lsp.git
cd angelscript-lsp

# 2. Configure and build C++ backend server
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Debug
cmake --build server/build --config Debug

# 3. Build VS Code TypeScript extension client
cd client
npm install
npm run compile
```

#### Linux / macOS (Bash)

```bash
# 1. Clone repository
git clone https://github.com/Gaftherman/angelscript-lsp.git
cd angelscript-lsp

# 2. Configure and build C++ backend server
cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build --config Release

# 3. Build VS Code TypeScript extension client
cd client
npm install
npm run compile
```

---

## Running Modular Unit & Integration Tests

The test suite uses Doctest and is split into modular executables compiled against `angel_lsp_core` and `doctest_main`.

To run all unit and integration tests via CTest:

```bash
cd server/build
ctest -C Debug --output-on-failure
```

### Test Executables Summary

| Executable Target | Description |
| :--- | :--- |
| `test_core` | Document tree creation, Tree-Sitter AST parsing, and script function/class definitions. |
| `test_analysis` | Symbol collection, symbol resolution, `as.predefined` loading, and validation oracle. |
| `RealIntegrationTests` | Integration tests verifying real `as.predefined` parsing and dual-pass syntax & semantic diagnostic catching. |
| `test_feature_hover` | Hover rendering, Markdown formatting, template rendering, and edge cases. |
| `test_feature_definition` | Goto definition and Goto type definition resolution. |
| `test_feature_semantic_tokens` | Full semantic token extraction and legend mapping. |
| `test_feature_completion` | Context-aware auto-completion items and scope filtering. |
| `test_feature_signature_help` | Function signature help and parameter index calculation. |
| `test_i18n` | Diagnostic localization and string translations for `en-US` and `es-ES`. |

---

## Command Line Configuration Flags

The `angel_lsp` executable accepts command-line flags to enable or disable individual LSP features at runtime:

| Flag | Description | Default |
| :--- | :--- | :--- |
| `--enable-hover=<bool>` | Enable or disable hover tooltips. | `true` |
| `--enable-definition=<bool>` | Enable or disable Goto Definition / Type Definition. | `true` |
| `--enable-completion=<bool>` | Enable or disable auto-completion. | `true` |
| `--enable-semantic-tokens=<bool>` | Enable or disable semantic token highlighting. | `true` |
| `--enable-signature-help=<bool>` | Enable or disable signature help. | `true` |
| `--locale=<lang>` | Set diagnostic language (`en-US` or `es-ES`). | `en-US` |

Example usage:

```bash
angel_lsp --enable-completion=true --enable-semantic-tokens=true --locale=es-ES
```
