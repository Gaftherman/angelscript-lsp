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
- **Go to Declaration (`textDocument/declaration`)**: The same answer as Go to Definition, deliberately. AngelScript has no declaration/definition split - no headers, no prototypes - so the two questions are one, and the editor's second navigation key should not be inert.
- **Go to Implementation (`textDocument/implementation`)**: The opposite direction. From an interface or a base class, the types that derive from it transitively; from a method declared in one, that method as each subtype declares it.
- **Expand Selection (`textDocument/selectionRange`)**: Grows the selection one syntactic step at a time, straight off the parse tree.
- **Call Hierarchy (`textDocument/prepareCallHierarchy`, `callHierarchy/incomingCalls`, `callHierarchy/outgoingCalls`)**: Who calls this function, and what it calls in turn, from a workspace-wide call index held beside the symbol table rather than inside it.
- **Type Hierarchy (`textDocument/prepareTypeHierarchy`, `typeHierarchy/supertypes`, `typeHierarchy/subtypes`)**: The bases a class or interface declares, and the types that declare it as theirs, one level at a time.
- **Linked Editing (`textDocument/linkedEditingRange`)**: Retype a local variable or a parameter and its uses together, live. Offered only for names a lexical scope keeps inside one file; anything at file scope goes through Rename, which looks across documents.
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
- **Configurable Preprocessor**: `#if`/`#endif` matching `CScriptBuilder` exactly, with `#else`, `#elif`, `#ifdef` and in-script `#define` available as opt-in switches for hosts that patched the add-on. Words come from `--define`, from the `angelscript.define` setting, or from a `#define` line in a predefined stub.
- **Active Predefined Stub**: One active predefined stub per workspace, chosen from a picker, with every other discovered stub ignored. The engine profile still loads underneath.
- **Engine Profiles**: standard, svencoop, urho3d, openxray, ootp, plus automatic detection.

---

## Language Surface Verified Against the Compiler

Every construct listed below was measured directly against the reference AngelScript compiler through `server/tools/oracle`, and each has a dedicated verification script in `server/tests/parity`. Rather than an aspirational promise, this list represents an empirical measurement that a test breaks if it ceases to hold true.

### Numeric literals
- Base prefixes: `0b1010`, `0o755`, `0d1024`, `0xFF00AA`
- Digit separators in every base: `1'000'000`, `0xDEAD'BEEF`, `0b1100'0011`*
- Leading-dot floats: `.30f`, `.5`, `.001`
- Scientific notation: `1.5e-3f`, `.2e+5`

### Operators
- Exponentiation `**` and `**=`
- Arithmetic right shift `>>>` and `>>>=`
- Logical xor, both spellings: `^^` and `xor`
- Keyword operators `and`, `or`, `not`
- Bitwise xor `^` and `^=`
- Handle identity `is` and `!is`
- The ternary `? :`

### Declarations and expressions
- Virtual properties in block form: `int Health { get const {...} set {...} }`
- Virtual properties through the `property` keyword: `int get_Health() const property`
- Lambdas whose parameter types are deduced from the target funcdef
- C++-style direct construction: `const Color red(1.0f, 0.0f, 0.0f);`
- Unscoped enum values

### Lexical edge cases
- `!isFlag` tokenises as `!` followed by an identifier, not as `!is` followed by `Flag`
- `property` and `super` used as ordinary identifiers, because they are contextual keywords
- A UTF-8 byte order mark at the start of a file

\* *Digit separators require the updated grammar (the LSP pins `tree-sitter-angelscript` by commit); until that pin is bumped, they are recorded as documented gaps in the parity audit.*

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

### The corpus audits

Nineteen test cases walk `angelscript/` — roughly 1,061 files of real, working third-party
AngelScript — and ask the one question this project treats as fatal: **does any rule report code
that compiles?** The unit suite cannot answer it, because it only ever asks whether a rule fires on
a snippet written to make it fire.

They are `skip()`-decorated, so `ctest` passes them over. Run them deliberately, and build
**Release** — one audit takes about 80 seconds optimised and roughly twenty minutes without:

```bash
cmake -B server/build-release -S server -DCMAKE_BUILD_TYPE=Release
cmake --build server/build-release --config Release --target angel_lsp_tests -j 8

server/build-release/Release/angel_lsp_tests --no-skip \
  --test-case="*Corpus Audit*,*Corpus Files*,The formatter keeps every token*"
```

The corpus is 13 MB of other people's scripts and is **not in this repository** — `.gitignore` has
excluded it from the start. Point `ANGELLSP_CORPUS_DIR` at wherever yours lives; without it the
audits report that they measured nothing and pass, rather than failing or, worse, auditing some
other directory and calling that a result.

`.github/workflows/corpus-audit.yml` runs the same command weekly and on demand. It stays green and
prints a notice until the `CORPUS_REPO` repository variable names a repository holding the corpus:

| Variable | Purpose |
|---|---|
| `CORPUS_REPO` | `owner/name` of the repository holding the corpus. Nothing runs without it. |
| `CORPUS_TOKEN` | A secret, only if that repository is private. |
| `CORPUS_REF` | Branch, tag or SHA, to pin the corpus to a known revision. |
| `CORPUS_SUBDIR` | Directory inside it, if the scripts are not at the root. |

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
| `CallHierarchyTest.cpp` | Layer 2/3 The call index itself - caller qualification, method calls, nested arguments - and the hierarchy built on it in both directions. |
| `LinkedEditingRangeTest.cpp` | Layer 3 Linked editing: shadowing, declaration-and-uses, and the file-scope names it refuses to offer. |
| `TypeHierarchyTest.cpp` | Layer 3 Type hierarchy in both directions, direct relations only, with the protocol's range-containment requirement pinned. |
| `ImplementationTest.cpp` | Layer 3 Go to Implementation: interfaces to implementing classes, base classes to derived ones transitively, methods to their overrides, and the cases that answer with nothing. |
| `SelectionRangeTest.cpp` | Layer 3 Expand selection chains: containment, no repeated links, and one answer per requested position. |
| `CallCheckerTest.cpp` | Layer 2 Argument counts at call sites, and the four shapes the corpus audit proved undecidable - mixin bodies, funcdef constructions, cross-plugin globals, unqualified names. |
| `ConstCheckerTest.cpp` | Layer 2 Const correctness at the use site: assigning to a const, and calling a non-const method through a const object. |
| `AccessCheckerTest.cpp` | Layer 2 Access control on member use: `private` per class, `protected` through a derived object, and the engine options that move the boundary. |
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
| `--enable-implementation[=true\|false]` | Enable or disable Go to Implementation. | `true` |
| `--disable-implementation` | Explicitly disable Go to Implementation. | - |
| `--enable-selection-range[=true\|false]` | Enable or disable expand selection. | `true` |
| `--disable-selection-range` | Explicitly disable expand selection. | - |
| `--enable-linked-editing[=true\|false]` | Enable or disable linked editing of locals. | `true` |
| `--disable-linked-editing` | Explicitly disable linked editing. | - |
| `--enable-call-hierarchy[=true\|false]` | Enable or disable call hierarchy. | `true` |
| `--disable-call-hierarchy` | Explicitly disable call hierarchy. | - |
| `--enable-type-hierarchy[=true\|false]` | Enable or disable type hierarchy. | `true` |
| `--disable-type-hierarchy` | Explicitly disable type hierarchy. | - |
| `--enable-type-conversion-checks[=true\|false]` | Enable or disable the type conversion diagnostics. | `true` |
| `--disable-type-conversion-checks` | Explicitly disable type conversion diagnostics. | - |
| `--enable-predefined-loader[=true\|false]` | Enable or disable background predefined symbols loader. | `true` |
| `--disable-predefined-loader` | Explicitly disable predefined symbols loader. | - |
| `--search-dir=<path>` | Add directory search path for `#include` resolution. | - |
| `--define=<WORD>` | Treat WORD as defined for `#if`. Mirrors `CScriptBuilder::DefineWord`. Repeatable. | - |
| `--preprocessor-feature=<name>=<value>` | Preprocessor extensions the host added to its own copy of CScriptBuilder. Names: `elseSupport`, `elifSupport`, `ifdefSupport`, `defineInScripts` (booleans) and `pragmaMode` (`accept`, `hint`, `error`). All off by default. Repeatable. | - |
| `--predefined-file=<path>` | Load a predefined stub by path, even from outside the workspace. Repeatable. | - |
| `--predefined-active=<path>` | Load only this predefined stub during the workspace scan. Empty loads every stub found, which is the historical behaviour. | - |
| `--diagnostic-severity=<code>=<severity>` | Override one diagnostic's severity: `error`, `warning`, `information` or `hint`. Repeatable. | - |
| `--engine-property=<name>=<value>` | Describe how the host built its engine — see below. Repeatable. | - |
| `--report-accessor-disabled` | Hint where a script property accessor is used but the host disabled those (`propertyAccessorMode` 0 or 1). | off |
| `--locale=<string>` | Set diagnostic language/locale. Any BCP 47 spelling works — only the primary subtag selects the table, so `es`, `es-ES` and `es-419` are equivalent. Unknown languages fall back to English. | `en` |
| `--file-ext=<string>` | Set AngelScript script file extension. | `.as` |
| `--predefined-ext=<string>` | Set predefined host API symbols file extension. | `.as.predefined` |
| `--array-like-type=<name>` | Name a template whose initializer list repeats its element type. Shorthand for a `@listpattern {repeat T}` tag — see below. Repeatable. | - |
| `-h`, `--help` | Show command-line help message and exit. | - |
| `-v`, `--version` | Show server version and exit. | - |

### Building against a local grammar checkout

The tree-sitter grammar is fetched by commit (`server/cmake/TreeSitter.cmake`). A grammar change and
the analyzer change that depends on it land together, and a pin cannot name a commit that has not
been pushed yet, so the build takes a local checkout instead when you point it at one:

```bash
cmake -B server/build -S server -DANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE=/path/to/tree-sitter-angelscript
```

Run `tree-sitter generate` in that checkout after editing `grammar.js` — the build compiles the
generated `src/parser.c` and does not run the CLI itself.

### Initializer List Patterns (`@listpattern`)

`{ ... }` is not a general-purpose initializer in AngelScript. A type accepts one only if the host
registered a **list factory** for it, and the shape it accepts is written into that registration:

```cpp
engine->RegisterObjectBehaviour("array<T>", asBEHAVE_LIST_FACTORY,
    "array<T>@ f(int&in type, int&in list) {repeat T}", ...);

engine->RegisterObjectBehaviour("dictionary", asBEHAVE_LIST_FACTORY,
    "dictionary @f(int &in) {repeat {string, ?}}", ...);
```

That trailing `{...}` is the only thing separating a type that takes `{1, 2, 3}` from one that takes
`{{"a", 1}}` from one that takes no list at all — and it is not derivable from anything else.
`optional<T>` is declared with the same single type parameter as `array<T>` and registers no list
factory, so the real compiler answers `optional<int> o = {1};` with
*"Initialization lists cannot be used with 'optional<int>'"*.

A predefined stub carries the pattern as a doc-comment tag, copied verbatim from the registration:

```angelscript
/// @listpattern {repeat T}
class array<T> { /* ... */ }

/// @listpattern {repeat {string, ?}}
class dictionary { /* ... */ }
```

With the tag present, the server reports a mismatched list the way the compiler does — including
inside nesting, so `array<int> a = {1, {2}};` and `dictionary d = {1, 2};` are both caught, while
`array<array<int>> g = {{1,2},{3,4}}` and `array<dictionary> a = {{{"a",1}}}` are correctly left
alone. Without it the server says nothing, because it cannot tell an absent list factory from a
stub that simply did not mention one.

A tag rather than a declaration because the pattern is not AngelScript source: written into a class
body, `{repeat T}` parses as a statement block containing a syntax error, which would put red
squiggles through your own stub. The built-in engine profiles already carry the tags for `array<T>`
and `dictionary`. `--array-like-type=<name>` is shorthand for `{repeat T}` if you would rather not
edit a stub you do not own.

Supported pattern syntax is AngelScript's own: `{...}` groups, `repeat` / `repeat_same`,
comma-separated sequences, type names, template parameters, and `?` for the variable type. A pattern
this server cannot parse is ignored rather than reported — a stub it cannot read is not your error
to see in your scripts.

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
| `propertyAccessorMode` | `3` (server: `2`) | Decides when `get_`/`set_` methods become virtual properties across four values: `0` (property accessors disabled outright), `1` (only accessors the application registered in C++; script ones are skipped), `2` (any `get_`/`set_` method is a property, with or without the `property` keyword), and `3` (only those carrying the `property` keyword; the engine's own default). This server uses `2` by default on purpose, not `3`: under `3` it would invent a diagnostic for every workspace whose host uses `2`, and falling short loses an error while over-reporting invents one. |

```bash
# A host that built its engine with unsafe references and isolated script state
angel_lsp --engine-property=allowUnsafeReferences=true --engine-property=disallowGlobalVars=true
```

In VS Code these live under `angelscript.engine.*`.

### Example Usage

```bash
# Start server with completion and semantic tokens, Spanish localization
angel_lsp --enable-completion=true --enable-semantic-tokens=true --locale=es-ES

# Disable predefined loader and signature help using space-separated flags
angel_lsp --disable-predefined-loader --enable-signature-help false

# Show CLI options
angel_lsp --help
```

