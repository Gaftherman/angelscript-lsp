# AngelScript Language Server (AngelLSP)

AngelLSP is a high-performance, thread-safe Language Server Protocol (LSP) implementation for the [AngelScript](https://www.angelcode.com/angelscript/) programming language (`.as` files). Built with C++20, it features a 100% pure **Tree-Sitter** & **SymbolTable** analysis architecture for instant response times and low memory footprint, paired with a Visual Studio Code extension client.

---

## Features

- **Pure Tree-Sitter Analysis Engine**: Complete AST parsing without native C++ engine binding callbacks or physical script concatenation.
- **Dual-Pass Diagnostics (`ValidationOracle`)**: 
  - **Syntax Pass**: Instant syntax error detection (`TSNode` error/missing node catching).
  - **Semantic Pass**: Workspace and document-level symbol resolution diagnostics.
- **Hover Information (`textDocument/hover`)**: Rich Markdown tooltips displaying function signatures, variable types, class properties, and parsed Doxygen documentation.
- **Go to Definition & Type Definition (`textDocument/definition`, `textDocument/typeDefinition`)**: Precise symbol lookup across documents, namespaces, classes, and global scopes with inheritance traversal.
- **Auto-Completion (`textDocument/completion`)**: Context-aware completion suggestions for global symbols, class member functions/properties, and namespace scopes.
- **Semantic Tokens (`textDocument/semanticTokens/full`)**: Full semantic syntax highlighting for keywords, types, functions, variables, parameters, and enum members.
- **Signature Help (`textDocument/signatureHelp`)**: Active parameter highlight and signature preview for function calls.
- **Document Symbols Outline (`textDocument/documentSymbol`)**: Hierarchical symbol tree for classes, methods, fields, enums, and namespaces powering VS Code Outline and Breadcrumbs.
- **Workspace Symbol Search (`workspace/symbol`)**: Multi-tiered fuzzy search across all translation units and predefined headers for fast `Ctrl+T` symbol navigation.
- **Find References (`textDocument/references`)**: Project-wide reference lookup for local variables, parameters, class members, and global declarations with shadowing protection.
- **Symbol Rename (`textDocument/prepareRename`, `textDocument/rename`)**: Safe identifier refactoring generating accurate multi-file `WorkspaceEdit` blocks.
- **Include Directive Resolution (`#include`)**: Preprocessor include extraction and resolution with search path configuration and cyclic dependency guards.
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

## Running Unit & Integration Tests

The test suite uses [doctest](https://github.com/doctest/doctest) and is compiled into a single unified test executable target, `angel_lsp_tests`, registered with CMake and CTest.

To run all unit and integration tests via CTest:

```bash
cd server/build
ctest -C Debug --output-on-failure
```

Or run the test executable directly:

```powershell
# Windows
server/build/Debug/angel_lsp_tests.exe

# Linux / macOS
server/build/angel_lsp_tests
```

### Test Suites Summary

| Test Suite File | Coverage Area |
| :--- | :--- |
| `UtilsTest.cpp` | Layer 1 primitives validation (`IsPrimitiveType`), predefined file matching (`IsPredefinedFile`), `Document` struct integrity. |
| `IncludeResolutionTest.cpp` | Layer 1 `#include` extraction, search path resolution, cyclic inclusion guards, and comment safety. |
| `SymbolCollectorTest.cpp` | Layer 2 AST symbol extraction (functions, classes, interfaces, enums, mixins, typedefs, funcdefs, variables, and syntax error diagnostics). |
| `LocalScopeCollectorTest.cpp` | Layer 2 lexical block scopes, nested compound statements, function/method parameters, and local variables. |
| `SemanticAnalyzerTest.cpp` | Layer 2 semantic diagnostics, undeclared identifier checks, type resolution, and `as.predefined` stub integration. |
| `ClassRulesTest.cpp` | Layer 2 class and interface declaration rules: inheritance cycles, final and multiple bases, unimplemented interface methods, mixin constraints. |
| `TypeRulesTest.cpp` | Layer 2 typedef, funcdef and enum declaration rules, plus redeclaration and cross-kind name conflicts. |
| `VariableRulesTest.cpp` | Layer 2 variable, field and virtual property rules: void and handle-on-primitive types, misplaced modifiers, accessor bodies. |
| `FunctionRulesTest.cpp` | Layer 2 function declaration rules: missing bodies, return types, member qualifiers, constructors and destructors, `override`, parameter lists. |
| `OperatorRulesTest.cpp` | Layer 2 operator overload rules: `opCmp` / `opEquals` return types, binary and index arity, placement inside a class. |
| `ControlFlowTest.cpp` | Layer 2 control-flow rules over the syntax tree: `break` / `continue` placement, `switch` clauses, and non-void functions that can fall off the end. |
| `TypeConversionTest.cpp` | Layer 2 conversion diagnostics for `T v = expr;`, `T(expr)` and `cast<T>(expr)` against constructors and the `opConv` / `opCast` family. |
| `RuleCostTest.cpp` | Layer 2 opt-in cost measurement of each analysis pass over 300 corpus files. |
| `SymbolTableIndexTest.cpp` | Layer 2 per-file bucket index, document replacement, and rule-index invalidation against the table version. |
| `I18nTest.cpp` | Layer 1 locale tag selection (`es`, `es-ES`, `es-419`, `es_MX`) and English/Spanish coverage of every emitted code. |
| `SignatureFormatterTest.cpp` | Layer 2 source-faithful declaration rendering: access modifiers, `const`, handles, and `&in` / `&out` / `&inout`. |
| `DocCommentTest.cpp` | Layer 2 Doxygen and line-comment documentation extraction shared by hover and completion. |
| `ServerHarnessTest.cpp` | Layer 4 end-to-end protocol coverage over a scripted in-memory transport: capabilities, diagnostics, watched files, workspace folders, semantic token deltas. |
| `HoverTest.cpp` | Layer 3 Hover tooltips, markdown rendering, Doxygen documentation extraction, and type signatures. |
| `DefinitionTest.cpp` | Layer 3 Go to Definition and Go to Type Definition across global, class member, and local symbols. |
| `CompletionTest.cpp` | Layer 3 Context-aware auto-completion (lexical variables, class members via `.` / `->`, enum members via `::`, keywords). |
| `SemanticTokensTest.cpp` | Layer 3 Full semantic tokens generation with delta-encoded integer streams and standard LSP token legends. |
| `SignatureHelpTest.cpp` | Layer 3 Function signature preview, parameter information, and active parameter indexing during call expressions. |
| `DocumentSymbolTest.cpp` | Layer 3 Hierarchical document symbol outline (classes, methods, fields, enums, namespaces, interfaces). |
| `WorkspaceSymbolTest.cpp` | Layer 3 Multi-tiered fuzzy search, scoring, and ranking across all indexed files. |
| `ReferencesTest.cpp` | Layer 3 Project-wide references lookup with lexical shadowing and class inheritance awareness. |
| `RenameTest.cpp` | Layer 3 Prepare rename validation and safe `WorkspaceEdit` generation across multiple documents. |
| `ServerConfigTest.cpp` | Layer 1/4 CLI argument parsing, boolean feature flag toggles, option syntax (`--flag=value` / `--flag value`), and robustness. |

---

## Command Line Configuration Flags

The `angel_lsp` executable accepts command-line arguments to enable or disable individual LSP features and configure runtime options. Both `--flag=value` and `--flag value` syntaxes are supported.

| Flag | Description | Default |
| :--- | :--- | :--- |
| `--enable-hover[=true\|false]` | Enable or disable hover tooltips. | `true` |
| `--disable-hover` | Explicitly disable hover tooltips. | - |
| `--enable-definition[=true\|false]` | Enable or disable Go to Definition / Type Definition. | `true` |
| `--disable-definition` | Explicitly disable Go to Definition. | - |
| `--enable-completion[=true\|false]` | Enable or disable context-aware auto-completion. | `true` |
| `--disable-completion` | Explicitly disable auto-completion. | - |
| `--enable-semantic-tokens[=true\|false]` | Enable or disable semantic syntax highlighting. | `true` |
| `--disable-semantic-tokens` | Explicitly disable semantic tokens. | - |
| `--enable-signature-help[=true\|false]` | Enable or disable signature help and active parameter index. | `true` |
| `--disable-signature-help` | Explicitly disable signature help. | - |
| `--enable-document-symbols[=true\|false]` | Enable or disable document symbols outline. | `true` |
| `--disable-document-symbols` | Explicitly disable document symbols outline. | - |
| `--enable-workspace-symbols[=true\|false]` | Enable or disable workspace symbol search. | `true` |
| `--disable-workspace-symbols` | Explicitly disable workspace symbol search. | - |
| `--enable-references[=true\|false]` | Enable or disable find references. | `true` |
| `--disable-references` | Explicitly disable find references. | - |
| `--enable-rename[=true\|false]` | Enable or disable symbol rename refactoring. | `true` |
| `--disable-rename` | Explicitly disable symbol rename. | - |
| `--enable-document-highlight[=true\|false]` | Enable or disable read/write occurrence highlighting. | `true` |
| `--disable-document-highlight` | Explicitly disable document highlight. | - |
| `--enable-folding-range[=true\|false]` | Enable or disable folding ranges. | `true` |
| `--disable-folding-range` | Explicitly disable folding ranges. | - |
| `--enable-inlay-hints[=true\|false]` | Enable or disable parameter-name and `auto` type hints. | `true` |
| `--disable-inlay-hints` | Explicitly disable inlay hints. | - |
| `--enable-code-action[=true\|false]` | Enable or disable quick fixes. | `true` |
| `--disable-code-action` | Explicitly disable code actions. | - |
| `--enable-formatting[=true\|false]` | Enable or disable document and range formatting. | `true` |
| `--disable-formatting` | Explicitly disable formatting. | - |
| `--enable-document-link[=true\|false]` | Enable or disable `#include` links. | `true` |
| `--disable-document-link` | Explicitly disable `#include` links. | - |
| `--enable-type-conversion-checks[=true\|false]` | Enable or disable the type conversion diagnostics. | `true` |
| `--disable-type-conversion-checks` | Explicitly disable type conversion diagnostics. | - |
| `--enable-predefined-loader[=true\|false]` | Enable or disable background predefined symbols loader. | `true` |
| `--disable-predefined-loader` | Explicitly disable predefined symbols loader. | - |
| `--search-dir=<path>` | Add directory search path for `#include` resolution. | - |
| `--predefined-file=<path>` | Load a predefined stub by path, even from outside the workspace. Repeatable. | - |
| `--diagnostic-severity=<code>=<severity>` | Override one diagnostic's severity: `error`, `warning`, `information` or `hint`. Repeatable. | - |
| `--engine-property=<name>=<bool>` | Describe how the host built its engine — see below. Repeatable. | - |
| `--locale=<string>` | Set diagnostic language/locale. Any BCP 47 spelling works — only the primary subtag selects the table, so `es`, `es-ES` and `es-419` are equivalent. Unknown languages fall back to English. | `en` |
| `--file-ext=<string>` | Set AngelScript script file extension. | `.as` |
| `--predefined-ext=<string>` | Set predefined host API symbols file extension. | `.as.predefined` |
| `-h`, `--help` | Show command-line help message and exit. | - |
| `-v`, `--version` | Show server version and exit. | - |

### Engine Properties

AngelScript is not one language but a family of them: the host picks its dialect with
`asIScriptEngine::SetEngineProperty` before it compiles anything, and several of those choices
decide whether a given line is legal. None of it is visible in script text, so a rule that depends
on one is undecidable until the host says which engine this is. `--engine-property` is where the
answer arrives.

Names are the `asEEngineProp` identifiers without their `asEP_` prefix, in lowerCamel, so a host
author can map its own `SetEngineProperty` calls across without translating anything. Every default
matches the engine's own, and only the properties a rule actually reads are accepted — an unknown
name is inert rather than an error.

| Property | Engine default | What it changes |
| :--- | :--- | :--- |
| `allowUnsafeReferences` | `false` | With it off, `&` on a parameter means `&inout` and only an object type that supports handles may use it, so `void f(int &x)` is reported. With it on, it is not. |
| `privatePropAsProtected` | `false` | A `private` member follows the `protected` rule, so a derived class may reach it. |
| `disallowGlobalVars` | `false` | Every global variable declaration becomes a compile error, and is reported as one. |

```bash
# A host that built its engine with unsafe references and isolated script state
angel_lsp --engine-property=allowUnsafeReferences=true --engine-property=disallowGlobalVars=true
```

In VS Code the same three live under `angelscript.engine.*`.

### Example Usage

```bash
# Start server with completion and semantic tokens, Spanish localization
angel_lsp --enable-completion=true --enable-semantic-tokens=true --locale=es-ES

# Disable predefined loader and signature help using space-separated flags
angel_lsp --disable-predefined-loader --enable-signature-help false

# Show CLI options
angel_lsp --help
```

