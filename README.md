# AngelScript Language Server (AngelLSP)

AngelLSP is a high-performance, thread-safe Language Server Protocol (LSP) implementation for the [AngelScript](https://www.angelcode.com/angelscript/) programming language (`.as` files). Built with C++20, it features a 100% pure **Tree-Sitter** & **SymbolTable** analysis architecture for instant response times and low memory footprint, paired with a Visual Studio Code extension client.

> [!WARNING]
> ### ⚠️ Project Status: Work in Progress (WIP)
> **AngelLSP is currently under active, heavy development and experimental validation.** While it already provides rich language intelligence, high-performance AST analysis, and continuous parity verification against the reference compiler, certain language constructs, edge cases, and diagnostics are still evolving.
>
> 💡 **Recommended Alternative for Production:**  
> If you need a battle-tested, mature language server for daily production work or mission-critical AngelScript projects right now, we **strongly and wholeheartedly recommend** using [**sashi0034/angel-lsp**](https://github.com/sashi0034/angel-lsp).

---

## What's New & Recent Updates (v0.6.2)

- **Dynamic Module Hot-Reloading**: Changes to `angelscript.modules` in `.vscode/settings.json` now reload dynamically via `workspace/didChangeConfiguration` without requiring a full server process restart or window reload.
- **Unconfigured Closure File Purging**: Removing a module from configuration immediately purges obsolete closure files, declarations, and symbols from the symbol table, ensuring accurate diagnostics when modules are removed or re-added.
- **Native `${workspaceFolder}` Path Resolution**: Server-side path resolution directly expands and normalizes `${workspaceFolder}` and `${workspaceRoot}` macros against the workspace roots for module entries and folder targets.

---

## What's New & Recent Updates (v0.6.1)

- **Folder-Based Module Support in Client**: VS Code client extension now forwards `--module-folder` to the server when `angelscript.modules` entries specify a `folder`, allowing entire directories of scripts (e.g. Sven Co-op maps or plugins) to be registered automatically.
- **Closure Cache & Symbol Retention**: The LSP server retains symbol table declarations and closure caches for configured module members across document open/close cycles, eliminating repetitive disk reads and UI freezes when switching between editor tabs.

---

## What's New & Recent Updates (v0.6.0)

- **Predefined Stubs in Editor & Live Incremental Editing**: Opening `.as.predefined` stub files directly in the editor now parses clean without false-positive errors; inline list-pattern notation (`{repeat T}`) is transparently rewritten on the document analysis path while keeping the client buffer mirror verbatim.
- **Rich Completion Snippets & Function Call Expansion**:
  - Dedicated declaration snippets for `enum`, `funcdef`, `switch`, `if`, `else`, `for`, `while`, `do`, `try`, and `#include "$1"`.
  - Function completion automatically inserts the call with parameter placeholders (`Function(${1:int arg})$0`) matching the symbol table signature.
- **Compiler Parity Validation (247 Scripts Clean)**:
  - Diagnostic for duplicate enum member declarations (`as-err-name-conflict`).
  - Validation of parameter names preventing collisions with non-contextual reserved keywords.
  - Clean sweep verification across all workspace predefined stubs.

---

## What's New & Recent Updates (v0.5.0)

- **Doxygen Documentation Parser (clangd Parity)**: Hover tooltips now feature an AST-based Doxygen docstring parser supporting `@brief`, `@param`, `@tparam`, `@return`, `@note`, `@warning`, `@see`, `@throw`, formatting identically to `clangd`.
- **Full Operator Overload System**: Complete validation of all 52 AngelScript operator overloads (`opCmp`, `opEquals`, `opAdd`, `opSub`, `opMul`, `opDiv`, `opIndex`, `opPostInc`, `opAssign`, etc.) and dynamic `cast<T>` expressions.
- **Enhanced Type & Expression Diagnostics**:
  - Rejection and diagnostic reporting for standalone anonymous functions/lambdas.
  - Support for ternary conditional expressions (`cond ? a : b`) as assignable l-values.
  - Fully qualified enum member names (`Enum::Member`) in symbol table and resolution.
  - Strict validation of parameter names preventing collisions with reserved keywords.
- **Predefined Stubs & Engine Extensions**:
  - Native support for standard AngelScript add-on types (`string`, `array<T>`, `dictionary`, `ref`, `datetime`, `file`).
  - Predefined stubs can specify list factories using manual constructor syntax `{repeat T}`.
  - Built-in Sven Co-op API stub (`predefned/sven.as.predefined`) and workspace stub selection.
  - **Format Predefined Stub** editor command: automatically consolidates duplicate namespace declarations from engine dumps while preserving 100% of comments and formatting.
- **Context-Aware Completion & Semantic Tokens**:
  - Completion prioritization offers declared class types before the `class` keyword.
  - Precise token classification for preprocessor directive lines.
- **Automated Parity & Multi-Platform Testing**:
  - Parity audit test suite (`server/tests/parity`) continuously measuring analyzer verdicts against the reference AngelScript compiler with over 213 test scripts.
  - Local Docker test harness (`docker/run_audit.sh`) for rapid parity testing on Linux.

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

The audit's current standing, from `server/tests/ParityAuditTest.cpp` run against the oracle:

| | |
| :--- | ---: |
| Scripts in the parity corpus | **213** |
| Agree, both accept | 114 |
| Agree, both reject | 96 |
| **Unexplained false positives** | **0** |
| False negatives, each with its reason recorded | 3 |

Zero is the only acceptable number in that fourth row: reporting an error on code the compiler
accepts is the one failure this project treats as fatal. The three false negatives are deliberate
and named in the audit's output - `doc_r06` (a class converting to bool through `opImplConv`, which
a host engine setting can make legal), `doc_r07` (an accessor without the `property` keyword) and
`doc_r13` (a base handle returned where a derived one is declared).

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

### Preprocessor directives

CScriptBuilder reads the characters immediately after the `#`, and anything it does not match is
left in the source for the compiler to reject. All of these were measured against the compiler and
are pinned by fixtures in `server/tests/parity`:

| Written | Verdict |
| --- | --- |
| `#include "helper.as"` | accepted |
| `#!/usr/bin/as` | accepted - a shebang is skipped, not a directive |
| `    #if FOO` ... `    #endif` | accepted - whitespace *before* the hash is fine |
| `#incude "helper.as"` | rejected - `as-err-unknown-directive` |
| `#` alone | rejected - `as-err-unknown-directive` |
| `#Include "helper.as"` | rejected - directive names are case-sensitive |
| `# include "helper.as"` | rejected - `as-err-directive-space-after-hash` |
| `#  if FOO` | rejected, and it excludes nothing |
| `#include helper.as` | rejected - `as-err-include-not-quoted`, the path needs double quotes |

The last one is the reason the pair matters: reading `#  if UNDEFINED` as a live directive would
drop the block below it, and every diagnostic inside a region the compiler actually keeps would go
with it.

`#else`, `#elif`, `#ifdef`, `#ifndef` and `#define` are recognised names that the stock add-on does
not support. Those stay **warnings**, not errors, because a host may genuinely have patched its own
copy of `scriptbuilder.cpp` - `angelscript.preprocessor.*` is how you say so. No setting makes a
misspelled name legal, which is why those two are errors.

Hovering an `#include` shows the file it actually resolves to - this file's own directory first,
then each `angelscript.searchDirectories` entry in order - or says plainly that it resolves to
nothing.

### Include paths

`#include` resolves against the including file's own directory first, then each
`angelscript.searchDirectories` entry in the order given.

Typing `#include "` completes with every script in the workspace, each offered as the path relative
to the file you are editing - a sibling as `helper.as`, a file one directory up as `../shared.as`,
one in a subdirectory as `weapons/rifle.as`.

By default the path is opened exactly as written, which is what AngelScript does: measured,
`#include "helper"` finds a file named literally `helper` and does **not** find `helper.as`. Some
hosts resolve the name themselves and require the extension to be left off - Sven Co-op works this
way, and there `#include "helper"` is the correct spelling. Set
`angelscript.include.implicitExtension` to `true` for those, and `helper` finds `helper.as`, with
completion inserting the short form to match.

The exact name is always tried first in each directory before the extension is appended, so a
directory holding both `helper` and `helper.as` resolves the way the compiler would.



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

Twenty-two test cases walk `angelscript/` — roughly 1,061 files of real, working third-party
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
| `GrammarNamesTest.cpp` | Layer 1 Every node type and field constant in `parser/GrammarNames.h` still resolves against the loaded tree-sitter language, so a grammar pin bump fails with a name in it rather than silently making rules stop matching. Includes the check that the check can fail. |
| `TypeVocabularyTest.cpp` | Layer 1/2 What the consolidated keyword, primitive and type-name helpers answer: reserved words against contextual ones, the primitive subsets, `::` qualification, and the two different empty-argument policies of the template argument splitter. |
| `ParityAuditTest.cpp` | Layer 2 The opt-in audit that compiles every script in `server/tests/parity` with the real AngelScript compiler and compares its verdict against this analyzer's. Fails on a single unexplained false positive. |
| `FormatterCorpusTest.cpp` | Layer 3 Formatting every parity script under both brace styles must lose no token and settle in one pass - the guard that caught a number and an identifier being welded into one. |
| `PredefinedStubAuditTest.cpp` | Layer 2 Every `as.predefined` in the checkout parses and analyses clean, so a stub cannot ship reporting errors about itself. |
| `DefiniteAssignmentTest.cpp` | Layer 2 Reading a local before it is written, across branches, loops and early returns. |
| `CodeActionTest.cpp` | Layer 3 Quick fixes and refactors: the edits they produce, the ranges they claim, and the positions where they must offer nothing. |
| `RefactoringTest.cpp` | Layer 3 Extract-method and its adversarial cases - captured variables, member access, loops with internal control flow. |
| `FormattingTest.cpp` | Layer 3 Spacing, indentation and brace placement in both styles, including the template-bracket and unary-operator cases the token stream cannot decide alone. |
| `InitializerListTest.cpp` | Layer 2 Initializer lists against a type's `@listpattern`, including nesting, repeats and the holes the engine allows. |
| `PositionEncodingTest.cpp` | Layer 1/4 UTF-8 and UTF-16 position arithmetic, which decides whether a range lands where the user's cursor is on any line holding a non-ASCII character. |
| `PreprocessorRegionsTest.cpp` | Layer 1 `#if` / `#ifdef` / `#else` region tracking and the inactive ranges the client dims. |
| `WorkspaceIncludeGraphTest.cpp` | Layer 2 The include closure across files: what a change invalidates, and cycles. |
| `SemanticTokenKindTest.cpp` | Layer 3 The token kind each name is coloured with, resolved through the symbol table rather than guessed from syntax. |
| `ExpressionTypeTest.cpp` | Layer 2 The type an expression resolves to, which every conversion, access and call rule is built on. |
| `OverloadResolutionTest.cpp` | Layer 2 Choosing between overloads by argument type, including the conversions that make two candidates ambiguous. |
| `LValueCheckerTest.cpp` | Layer 2 What may appear on the left of an assignment. |
| `DocumentHighlightTest.cpp` | Layer 3 Read and write classification of a name's occurrences, lexical shadowing, and the boundaries the search must not cross. |
| `InlayHintTest.cpp` | Layer 3 Parameter-name and deduced-type hints, and the positions that must show none. |
| `FoldingRangeTest.cpp` | Layer 3 Folding for blocks, comments, `#region` markers and preprocessor branches. |
| `DocumentLinkTest.cpp` | Layer 3 `#include` paths as clickable links, resolved against the search directories. |
| `CodeLensTest.cpp` | Layer 3 The lenses offered above a declaration and what they resolve to. |
| `PredefinedFixtureTest.cpp` | Layer 2 Stub loading: multiple stubs, a selected one, a deleted one, and the engine profile that survives a selection. |
| `WorkspaceScanTest.cpp` | Layer 4 The workspace walk: what it indexes, what it excludes, and what a rescan replaces. |
| `WorkspaceStubSweepTest.cpp` | Layer 4 The sweep that finds every stub in a workspace, by suffix and by name. |
| `RenameReferencesParityTest.cpp` | Layer 3 Rename and find-references must agree on the same set of occurrences; a difference between them is a rename that misses one. |
| `DeepNestingTest.cpp` | Layer 2 Deeply nested constructs against the traversal depth caps, so a pathological file degrades rather than crashes. |
| `SemanticHelpersTest.cpp` | Layer 2 The shared type-name helpers in isolation. |
| `AdversarialMilestone1Test.cpp`, `AdversarialSemanticsM1Test.cpp`, `AdversarialFeaturesTest.cpp`, `AdversarialChallenger1Test.cpp`, `AdversarialPhase3FeaturesTest.cpp`, `AdversarialPhase3Challenger1Test.cpp`, `AdversarialRefactoringPhase2Test.cpp`, `AdversarialRefactoringM2ChallengerTest.cpp` | Layer 2/3 Cases written to break the analyzer rather than to demonstrate it - malformed input, hostile nesting, and the shapes each milestone's rules were most likely to get wrong. |

---

## Script modules

A module is AngelScript's own unit of compilation - what `builder.StartNewModule(engine, name)`
creates - and everything its entry script pulls in through `#include` belongs to it. The server
cannot deduce this: a directory of scripts may be one module or one module per file, and only the
host knows which. So you say:

```jsonc
{
  "angelscript.modules": [
    { "name": "shared", "entry": "${workspaceFolder}/scripts/shared_main.as" },
    { "name": "server", "entry": "${workspaceFolder}/scripts/server_main.as" }
  ]
}
```

Empty is the default, and empty changes nothing.

What it buys is `external shared`. Measured:

| Written | Verdict |
| --- | --- |
| `external shared class Foo;`, and another module declares `shared class Foo` | accepted |
| `external shared class Foo;` and nothing declares it | rejected |
| `external shared class Foo;` **and `shared class Foo {}` in the same module** | rejected |

The third row is the one worth reading twice. A full definition sitting in the same module does not
satisfy an external declaration - `external` means "built elsewhere", and elsewhere means another
module. So the question is not "does this name exist", which a symbol table can answer, but "does it
exist somewhere else", which it cannot.

Without `angelscript.modules` the server asks the older, laxer question and accepts the false
negative, rather than reporting correct code as broken in whichever way it guessed wrong. With the
modules described, it asks the right one.

Configuring modules also makes the server read every file of every module, not only the closure of
whatever document is open - two modules are by definition not connected by an `#include`, so
nothing else would ever bring them into the same table.

`import void F() from "other";` is checked too, but only as a **hint**: measured, the compiler
accepts an import naming a module that was never built, because an imported function is bound at
run time. All the server can say is that the name matches none of the modules you described.

The workspace-wide predefined-stub loader is unaffected and can be turned off on its own with
`angelscript.features.predefinedLoader`.

### A folder as a module

A module entry may name a `folder` instead of, or as well as, an `entry` — which is how Sven Co-op
lays its scripts out, a directory deciding membership with no entry point to name:

```jsonc
{
  "angelscript.modules": [
    { "name": "MapScript", "folder": "${workspaceFolder}/scripts/maps" },
    { "name": "Plugin",    "folder": "${workspaceFolder}/scripts/plugins" }
  ]
}
```

Right-click a `.as` for **Set as Module Entry Point**, or a folder for **Set as Module Folder**;
both write into this setting with `${workspaceFolder}` so it stays portable.

A file belongs to exactly one module, and the most specific claim wins:

```
an entry point's include closure   >   the deepest folder   >   any folder above it
```

So `scripts/maps` as `MapScript` and `scripts/maps/deep` as `DeepMaps` puts a file in `deep` into
`DeepMaps`, the way a nested `.gitignore` works. AngelScript really does allow one file to be
compiled into several modules, so this is a simplification — and when more than one module claims a
file the server says so, naming the one it chose and the ones that lost. A rule that picks silently
is a rule nobody can check.

**Naming a module publishes diagnostics for every file in it**, not only the one you have open, so
an error inside an `#include`d file reaches the Problems panel and can be clicked through. Those
are recomputed when you save and when the workspace is scanned — not on every keystroke, which
would re-analyse a few hundred files after each typing pause. A file that leaves a module has its
diagnostics withdrawn; without that a renamed module would leave ghosts in the panel for the rest
of the session.

A configured module folder joins the roots an `#include` may resolve into. That is a deliberate
widening of the confinement that stops `#include "../../../etc/passwd"` — naming a folder as a
module is the statement that it is part of the project.



## Formatting a predefined stub

A stub generated from an engine's registration table emits one declaration per registered entity, so
a namespace with twenty members arrives as twenty namespaces. Sven Co-op's own stub declares
`Schedules` twenty times and `Hooks::Player` seventeen:

```angelscript
//Empty string. Useful when a reference to a string is needed.;
namespace String { const string EMPTY_STRING; }
//Default comparison type.;
namespace String { const CompareType DEFAULT_COMPARE; }
```

That is legal, and the analyzer reads it perfectly well. Nobody else can. Right-click the stub -
in the explorer or in the editor - and choose **Format Predefined Stub**:

```angelscript
namespace String
{
	//Empty string. Useful when a reference to a string is needed.;
	const string EMPTY_STRING;
	//Default comparison type.;
	const CompareType DEFAULT_COMPARE;
}
```

Every namespace comes out in that form, whether or not it had anything to merge, because a file
where some are blocks and others are one-liners is not formatted. Everything else keeps its bytes: a
class, an enum, a funcdef and every global are copied across untouched, so the command cannot
rewrite the parts of a hand-written stub it was not asked about.

Three properties it is worth knowing hold, because each has a test:

- **A comment never parts company with its declaration.** The text between one declaration and the
  next travels with the declaration below it, re-indented to its new depth. That includes blank
  lines and anything the parser did not recognise.
- **A file that does not parse is returned untouched.** Declaration boundaries in a broken file are
  guesses, and moving text on a guess is how a formatter eats someone's work.
- **Formatting twice changes nothing the second time.** The server answers `changed: false` and the
  editor is told the stub is already formatted.

Measured on Sven Co-op's 15,881-line stub: 79 namespace declarations become 14, and not one
declaration line is lost.

## Planned work

Two requests about *which files this server considers part of the program* - a root script, and
opening a `.as` from outside the workspace - are designed but not implemented.

Today, when a script is opened from outside every workspace folder: it is analysed, but its
`#include` of a file beside it is refused by the resolver's root allow-list, so everything that
file declares is reported as an unresolved type.

Folder modules - `scripts/maps` as `MapScript`, `scripts/plugins` as `Plugin`, the way Sven Co-op
lays them out - and module-wide diagnostics, so an error in a file the entry point includes reaches
the Problems panel instead of waiting until you open that file, are planned for future milestones.

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

### Initializer List Patterns

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

A predefined stub may carry the pattern either way. The preferred form writes the list factory as
the constructor it is, exactly as the AngelScript manual documents one:

```angelscript
class array<T>
{
    array();                                        // asBEHAVE_FACTORY
    array(uint length) explicit;                    // asBEHAVE_FACTORY
    array(int &in type, int &in list) {repeat T};   // asBEHAVE_LIST_FACTORY
}

class dictionary
{
    dictionary();
    dictionary(int &in type, int &in list) {repeat {string, ?}};   // asBEHAVE_LIST_FACTORY
}
```

The older form is a doc-comment tag above the class, and still works — every stub already written
keeps working:

```angelscript
/// @listpattern {repeat T}
class array<T> { /* ... */ }
```

With the tag present, the server reports a mismatched list the way the compiler does — including
inside nesting, so `array<int> a = {1, {2}};` and `dictionary d = {1, 2};` are both caught, while
`array<array<int>> g = {{1,2},{3,4}}` and `array<dictionary> a = {{{"a",1}}}` are correctly left
alone. Without it the server says nothing, because it cannot tell an absent list factory from a
stub that simply did not mention one.

The inline form is not AngelScript source — measured, both the compiler and this server's parser
read `{repeat T}` as a statement block declaring a variable `T` of type `repeat`. A stub is never
compiled by AngelScript, so it is allowed to spell things the language does not; what it may not do
is reach the parser that way. Predefined files, and only predefined files, therefore go through a
pre-pass that blanks the pattern and re-states it as a tag on the same line, leaving every line and
every column of every declaration where it was. Write it in an ordinary script and you still get the
syntax error the compiler gives you.

The built-in engine profiles carry the patterns for `array<T>`
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

