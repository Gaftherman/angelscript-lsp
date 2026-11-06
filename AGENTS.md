# Standard Operating Procedure (SOP) & Engineering Standard - AngelLSP

AngelLSP is a high-performance C++20 Language Server Protocol (LSP) implementation for the AngelScript language (`.as`), powered by Tree-Sitter for AST parsing, semantic token resolution, and symbol analysis.

---

## 1. Architectural Layers & Include Matrix

Layer isolation is strict and enforced by `server/scripts/check-layer-includes.py`:

| Layer | Path | Allowed to `#include` | Strictly FORBIDDEN to `#include` |
| :--- | :--- | :--- | :--- |
| **Layer 1: Core / Config** | `core/`, `config/`, `document/`, `parser/`, `utils/` | Own layer, standard C++ libraries | Layers 2, 3, and 4 |
| **Layer 2: Analysis** | `analysis/` | Layer 1, standard C++ libraries | Layers 3 and 4 |
| **Layer 3: Features** | `features/<feature_name>/` | Layers 1 and 2 | Sibling features, Layer 4 |
| **Layer 4: Server / LSP** | `lsp/`, `main.cpp` | Layers 1, 2, and 3 | None (topmost layer) |

- **Service Placement in Layer 2:** Semantic resolution services (`TargetResolution`, `TypeResolution`, `ScopeLookup`, `OverloadResolver`) belong strictly in Layer 2 (`analysis/`). Feature handlers in Layer 3 (`features/`) must never import sibling features or duplicate data structures (`TargetDescriptor`).
- **Protocol Isolation:** `analysis/` and `parser/` must compile without the LSP protocol library (`<lsp/...>`). Shared utilities such as `utils/LspLogger.h` and `i18n/i18n.h` may be included directly.

### Sub-Layer Domain-Driven Design (DDD) Architecture

#### Layer 2: Analysis (`server/src/analysis/`)
- **Sub-Layer 2.1 (Index):** `analysis/index/` - AST indexing and lexical structure (`NodeIndex`, `SymbolTable`, `SymbolCollector`, `ScopeTree`, `LocalScopeCollector`, `DeclarationRecovery`, `CallGraph`, `SemanticNodes`).
- **Sub-Layer 2.2 (Types):** `analysis/types/` - Type representation and engine profiles (`TypeExtraction`, `TypeSanitization`, `ExpressionTypeCache`, `EngineProfiles`, `EngineConfigConverter`, `VirtualMixinContext`).
- **Sub-Layer 2.3 (Diagnostics):** `analysis/diagnostics/` - Diagnostic error codes and context (`Diagnostics`, `DiagnosticCodes`, `DiagnosticContext`, `DiagnosticSuppression`).
- **Sub-Layer 2.4 (Documentation):** `analysis/documentation/` - Doc comment parsing and markdown formatting (`DocComment`, `DoxygenMarkdown`, `SignatureFormatter`).
- **Sub-Layer 2.5 (Resolution):** `analysis/resolution/` - Semantic target and overload resolution (`SemanticHelpers`, `TargetResolution`, `OverloadResolver`, `AmbiguityDetector`, `ConversionRankingEngine`, `OverloadTypeConversions`).
- **Sub-Layer 2.6a (Rules):** `analysis/rules/` - Semantic language rules (`RuleIndex`, `ClassRules`, `EngineRuleChecker`, `FunctionRules`, `OperatorRules`, `TypeRules`, `UnusedSymbolChecker`, `VariableRules`).
- **Sub-Layer 2.6b (Checkers):** `analysis/checkers/` - Semantic verification checkers (`AccessChecker`, `BinaryOperatorChecker`, `BinaryOperatorHelpers`, `CallChecker`, `ComparisonOperatorChecker`, `ConstChecker`, `ControlFlowChecker`, `DefiniteAssignmentChecker`, `HandleComparisonChecker`, `InitializerListChecker`, `IsolationChecker`, `LValueChecker`, `NamespaceChecker`, `RepeatedConversionChecker`, `TypeConversionChecker`, and `null_safety/` [`NullSafetyTypes`, `NullSafetyChecker`, `NullSafetyCondition`, `NullSafetyDecl`, `NullSafetyExpr`, `NullSafetyParam`]).
- **Sub-Layer 2.7 (Mediator & Coordinator):** `analysis/mediator/` (`SemanticMediator`) and `analysis/` root - Semantic coordinator facade (`SemanticAnalyzer`, `SemanticAnalysisRequest`).
- **Mandatory #include Top-Placement & Ordering (Rule 1.2):** All project headers (`#include "..."`) must be placed strictly before system/library headers (`#include <...>`) at the top of every C++ file. No `#include` directives are permitted after C++ declarations or code.
- **Mandatory SemanticMediator Pattern (Rule 1.3):** Checkers and resolution flows in Layer 2 must coordinate with `SymbolTable`, `ScopeTree`, and `NodeIndex` through the `SemanticMediator` facade (`analysis/mediator/SemanticMediator.h`).

#### Layer 4: Server / LSP (`server/src/lsp/`)
- **Sub-Layer 4.1 (State):** `lsp/state/` - In-memory LSP document and workspace state (`DocumentStore`, `WorkspaceStateStore`, `ModuleIndex`, `PredefinedStubManager`, `PositionCodec`).
- **Sub-Layer 4.2 (Pipeline):** `lsp/pipeline/` - Scheduling and diagnostics pipeline (`AnalysisScheduler`, `DiagnosticsPipeline`, `LspSessionCoordinator`).
- **Sub-Layer 4.3 (Services):** `lsp/services/` - Internal domain server services (`ServerAnalysis`, `ServerDiagnostics`, `ServerEngineRules`, `ServerModules`, `ServerPredefined`).
- **Sub-Layer 4.4 (Handlers):** `lsp/handlers/` - Protocol request and notification handlers (`BreakpointHandlers`, `DebugExpressionHandlers`, `HierarchyHandlers`, `TextDocumentHandlers`, `TextDocumentSyncHandlers`, `TokensAndFormattingHandlers`, `WorkspaceHandlers`, `WorkspaceScanHandlers`).
- **Sub-Layer 4.5 (Server Core):** `lsp/` root - Server core coordinator (`Server`).

---

## 2. Concurrency & AST Memory Invariants

1. **No Raw Tree Borrowing:** `DocumentStore` must never return a raw `TSTree*` that outlives its reader lock. Functions accessing ASTs must retain a `std::shared_ptr<const document::Document>` handle or operate on thread-local copies created via `ts_tree_copy()`.
2. **No Uncaught Thread Exceptions:** All secondary threads (`std::thread`, `AnalysisScheduler`, `WorkspaceScan`) must wrap execution in top-level `try/catch` handlers that log fatal errors to disk via `MultiFileLogger` and shut down gracefully without invoking `std::terminate()`.
3. **Mandatory Safe AST Navigation:** Always verify `!ts_node_is_null(node)` before invoking `ts_node_type()`, `ts_node_start_byte()`, or accessing child nodes.
4. **AST Thread Safety Invariant:** `TSTree*` pointers must never be shared across threads without `ts_tree_copy()`. Concurrency suites must validate simultaneous mutation and reading under randomized workloads.

---

## 3. Function Signature & Parameter Governance

1. **Parameter Ceiling:** Functions must accept at most 4 parameters. If more inputs are needed, bundle them into an immutable struct (e.g., `struct <Feature>Request`).
2. **Owned Signatures (Zero Dead Parameters):** Free functions, private helpers, and internal classes under our control must have zero dead parameters (`/we4100`). Unreferenced formal parameters must be removed from the header, implementation, and all call sites immediately. Nameless parameters (`Type`) and commented-out names (`Type /*name*/`) remain strictly prohibited.
3. **Contract-Mandated Signatures:** Where signatures are dictated by external APIs (virtual method `override`, Tree-Sitter C callbacks, generic STL lambda visitors), silence unused parameters explicitly via `[[maybe_unused]] Type name`. Keep parameter names intact for Doxygen and IDE hover introspection without requiring mandatory boilerplate `@note` comments.
4. **No Unnamed or Commented-Out Parameters:** Never bypass warnings using `int /* b */` or unnamed parameters (`int`). In owned signatures, delete unused parameters; in contract-mandated signatures, use `[[maybe_unused]] Type name`.

---

## 4. Tree-Sitter Traversal & Anti-Monolith Mandate

1. **Structure extraction:** Reuse the first-pass cursor or its index for document-wide work. Keep precompiled S-expression queries for targeted extraction and collectors awaiting migration. Avoid nested index-based `ts_node_child` loops.
2. **Flat Cursor Traversal:** When full sub-tree iteration is necessary, use flat `TSTreeCursor` loops (`ts_tree_cursor_goto_first_child` / `ts_tree_cursor_goto_next_sibling`) without recursion on the C++ execution stack.
3. **AST-First Feature Queries:** Feature handlers must query Tree-Sitter AST nodes or Layer 2 analysis APIs. Ad-hoc text lexers (`ConsumeIdentifier`, manual string slicing) are strictly forbidden in Layer 3.
4. **Cohesive C++ Design (Lizard Eliminated):** Lizard is permanently removed. Arbitrary function line limits, parameter ceilings, or cyclomatic limits enforced by lizard are eliminated. Functions must remain cohesive and readable without artificial fragmentation into micro-functions or synthetic parameter bags. Complex logic must be designed using clean domain abstractions, expressive structs, clean inheritance/interfaces where applicable, clean overloads, and mandatory Javadoc-style Doxygen documentation.
5. **Anti-Monolith & 300-Line Limit:** Production source files (`.cpp`) must target $\le 300$ net lines of code. Monolithic files exceeding this ceiling must be decomposed into cohesive, single-responsibility sub-units.
6. **No Root-Cause Bypass:** Never patch edge cases using string comparison hardcoding (`if (name == "...")`). Fixes must reside in grammar queries, symbol resolution passes, or type rules.
7. **AST Traversal & Parent-Climb Invariants:** Upward AST traversal must never use unbounded `while (!ts_node_is_null(ts_node_parent(node)))` loops to climb to the root; always use `parser::GetRootNode(node)` in $O(1)$ via `ts_tree_root_node(node.tree)`. Hot-path collectors and query passes (`LocalScopeCollector`, `SymbolCollector`, token classification) must never perform iterative `ts_node_parent` climb loops or string comparisons (`ts_node_type`); use $O(1)$ stack/byte-range containment, pre-cached `TSSymbol` comparisons, or bounded hops (`depth < 16`). Zero `ts_node_parent` calls are permitted in hot-path collectors.
8. **Official Layer Architecture Tool Matrix:**
   - Use `ScopeTree::FindInnermostScope(byte)` for local definitions and the lexical records in `NodeIndex` for container and namespace import lookup. The positional lookup uses binary search followed by enclosing links. Its cost includes the scopes visited and the names returned. A namespace import applies within its owning lexical scope.
   - **AST Navigation & Parent Isolation:** Upward traversal must strictly use `parser::GetParent(node)` (which safely verifies `!ts_node_is_null`). Calling raw `ts_node_parent` is strictly forbidden outside `server/src/parser/ASTUtils.h`. Root retrieval must use `parser::GetRootNode(node)` in $O(1)$.
   - **Scoped Name Manipulation:** Use `utils::ScopedNameUtils` (`StripGlobalPrefix`, `HasLeadingGlobalPrefix`, `ParentScopeView`, `SplitScopedName`, `ForEachEnclosingScope`, `AreEquivalentGlobalOrScopedNames`) operating entirely on `std::string_view` with zero heap allocations.
   - **Zero-Allocation Symbol Table Lookups:** Member lookups must use `SymbolTable::FindMemberSymbolPtr(container, name)` or `SymbolTable::FindMemberSymbols(container, name)`. Reachability in enclosing scopes must use `SymbolTable::HasScopedSymbol(scope, name)` or `SymbolTable::IsReachableInContainerScope(name, containerScope)`. Ad-hoc string concatenations like `+ "::" +` passed into symbol search APIs are strictly prohibited.
   - **Structure Extraction:** Use precompiled `GrammarSymbols` and `NodeIndex::ForEachNodeOrdered(std::span<const TSSymbol>)` without string conversions or runtime map lookups.
   - **Cross-Feature Protocol Helpers:** Use `features/common/FeatureSharedUtils.h` (`ToLspRange`, `SameRange`, `FindIdentifierAtPosition`, `ToLspSymbolKind`) to prevent duplicate mapping or geometry routines across feature handlers.
   - **Type Qualifiers & Canonical Sanitization:** Use `analysis::TypeQualifiers` and `analysis/TypeSanitization.h` (`StripLeadingConst`, `StripTrailingDecorationsView`, `TrimTypeWhitespace`) for zero-allocation AST-based type modifier operations.
9. **Pass-1 context capture (Rule 4.9):** One document-wide extraction visits $O(N)$ nodes per syntax revision. Direct access by a captured node ID can be $O(1)$. State the actual cost of positional queries and operations returning multiple results. Second-pass consumers migrated to indexed facts must use those facts and worklists for flow and expression analysis. The AST traversal gate checks direct navigation calls in migrated checkers. Runtime measurement must also cover calls through their dependencies. Normal LSP analysis obtains its AST-backed revision through `ResolveSyntaxRevision`. Reuse an exact parser input on the owning thread. Retained syntax stays within the cache budget, and closed entries yield space to open revisions.
10. **Strict First Pass vs. Second Pass Boundary (Rule 4.10):**
    - **First Pass (`server/src/parser/` and `server/src/analysis/index/`):** The ONLY location where `tree-sitter` (`TSNode`, `TSTree`, `TSTreeCursor`, `TSPoint`, `ts_*`) and `ASTUtils.h` are allowed. `NodeIndex` is strictly prohibited from exposing raw `TSNode` pointers (`->Root()`, callbacks accepting `TSNode`, or raw AST traversals) to Second Pass consumers. All indexed facts, symbols, spans, and tokens must be pre-extracted into immutable, AST-free structures.
    - **Second Pass (`features/`, `analysis/resolution/`, `analysis/checkers/`, `analysis/rules/`, `lsp/`) and `utils/`:**
      - Strictly 0 `ts_*` calls, 0 `TSNode` / `TSTree` usages, 0 `#include "parser/ASTUtils.h"`.
      - Strictly 0 legacy unbounded recursion; use $O(1)$ / $O(\log N)$ indexed lookups or flat iterations over pre-indexed First Pass vectors.
      - Strictly 100% `std::string_view` for string inspection, parameters, and paths (prohibited `std::string` and `const std::string&` where zero-copy views into `Document.h` or `SymbolTable` apply). Enforced via `server/scripts/verify_pass2_invariants.py --strict-string-view`.
11. **Shared Utility Functions: Overload & Deprecate Strategy (Rule 4.11):**
    - **Absolute Inlining Prohibition:** Never copy-paste or inline the body of a shared utility function into its call sites (causes immediate violations of `jscpd <= 3%`).
    - When a utility function in `utils/`, `features/common/FeatureSharedUtils.h`, `analysis/resolution/SemanticHelpers.h`, or `analysis/index/NodeIndex.h` uses `ts_*` or `std::string`:
      1. Inspect and trace all callers using `clangd` (`find_references`).
      2. Precalculate any required AST facts in First Pass (`NodeIndex`, `SymbolCollector`, `LocalScopeCollector`).
      3. Create a clean **overload** accepting First Pass data and `std::string_view`.
      4. Migrate each call site sequentially to the clean overload.
      5. Delete the legacy overload only when its reference count drops strictly to zero.

---

## 5. Doxygen Documentation Standard

All public classes, structs, member variables, functions, and feature contracts must be documented in **English** using Javadoc-style blocks (`/** ... */`):
- `@brief`: Concise single-line summary ending with a period.
- `@param[in/out]`: Explicit direction tag, parameter name, and purpose.
- `@return`: Detailed return description, including `std::nullopt` or empty-state semantics.
- `@note` / `@warning`: Concurrency guarantees or AST node lifetime rules.

---

## 6. Conventional Commits & Git Hygiene

Format: `<type>(<scope>): <short imperative description>`
- **Types:** `feat`, `fix`, `test`, `perf`, `refactor`, `style`, `docs`, `chore`.
- **Scopes:** `core`, `parser`, `analysis`, `features`, `server`, `harness`, `tests`, `docs`.
- Zero debug code: Never commit `std::cout`, `printf`, or temporary tracing logs.

---

## 7. Invariant-Based Testing & Anti-Overfitting Governance

1. **Generalizable fixtures:** Use generated identifiers and paths to exercise semantic invariants. Use fixed fixtures for precise regressions or external corpus parity, and record seeds when generated structure affects reproduction.
2. **Path Containment Invariant:** Path resolution must be tested using dynamic temporary sandboxes with multi-depth randomized traversal sequences. Testing only fixed strings like `../../etc/passwd` is strictly prohibited.
3. **Transport Security Invariant:** JSON-RPC transport layers must enforce a bounded envelope (`MAX_LSP_PAYLOAD_SIZE = 16 MB`). Payloads outside bounds or with malformed headers must be rejected without allocations or crashes.
4. **Topological Graph Invariant:** Invalidation traversals over `WorkspaceIncludeGraph` must be tested on randomized DAGs asserting strictly $O(V + E)$ deduplicated visits.

### Deterministic Concurrency & Anti-Flakiness Testing Invariant

1. **Absolute Ban on Arbitrary Sleep and Polling:**
   - Tests must NEVER use `std::this_thread::sleep_for()`, `std::this_thread::yield()` in loops, or counter-based polling loops (`for (int i = 0; i < N; ++i) { sleep_for(...); }`).
   - Tests must NEVER rely on elapsed time or timing thresholds to assert that an asynchronous task has started, completed, or failed.
2. **Event-Driven Synchronization Mandate:**
   - All multi-threaded and asynchronous assertions must synchronize strictly via deterministic event primitives:
     - `std::promise<T>` and `std::future<T>::get()` for one-shot task completions and exception assertions.
     - `std::binary_semaphore` or `std::counting_semaphore` for explicit signal handoffs.
     - `std::latch` or `std::barrier` for multi-thread rendezvous points.
   - Execution must be immediate: when the background worker signals completion, the waiting thread unblocks with zero artificial latency.
3. **Zero In-Test Timeout Logic:**
   - Never write watchdog timers, sleep loops, or timeout retry loops inside test cases.
   - Hang and deadlock protection is strictly external and managed by CMake test properties:
     `set_tests_properties(<target> PROPERTIES TIMEOUT 10)`

---

## 8. The 15 Absolute Prohibitions

1. **PROHIBITION 1: No Unprotected Raw AST Pointer Escaping:** Never return or store raw `TSTree*` pointers without an enclosing reader lock or owning `std::shared_ptr<const document::Document>`.
2. **PROHIBITION 2: No Uncaught Exceptions Escaping Secondary Threads:** Never allow an exception to escape a `std::thread` boundary; unhandled exceptions that invoke `std::terminate()` are classified as critical severity bugs.
3. **PROHIBITION 3: No Cross-Feature Includes (Layer 3 Isolation):** Never include headers from sibling features in `server/src/features/`. Cross-cutting capabilities must be factored into Layer 2 (`analysis/`).
4. **PROHIBITION 4: No Duplication of Core Analysis Structures:** Never copy and paste core data models (`TargetDescriptor`, `TargetKind`) to circumvent Layer Matrix rules.
5. **PROHIBITION 5: No Ad-Hoc Text Lexers in Layer 3:** Never write custom string slicing lexers (`ConsumeIdentifier`, index-based paren counting) to inspect expressions; always use Tree-Sitter AST nodes or Layer 2 type resolution.
6. **PROHIBITION 6: No Index-Based `ts_node_child` Traversal Loops:** Structure extraction must use precompiled S-expression queries or flat cursor traversal.
7. **PROHIBITION 7: No Stack-Recursive AST Traversals:** Use iterative cursor traversal or explicit worklists. A budget that prevents completion must surface a partial result. Arbitrary depth limits must not silently discard valid input.
8. **PROHIBITION 8: No Artificial Function Fragmentation or Synthetic Parameter Bags:** Never fragment cohesive domain functions into unreadable micro-routines or introduce artificial wrapper structs ("parameter bags") to circumvent line/parameter limits. Use idiomatic C++ design in First Pass (rich domain structs, clear overloads, interfaces/inheritance where applicable) with complete Doxygen documentation.
9. **PROHIBITION 9: No Dead or Unnamed Parameters (`/we4100`):** In owned signatures, every formal parameter must be named and referenced, or cleanly removed. In contract-mandated signatures (virtual overrides, external callbacks), unused parameters must be named and explicitly silenced with `[[maybe_unused]] Type name`. Nameless (`Type`) or commented-out (`Type /*name*/`) parameters are strictly banned.
10. **PROHIBITION 10: No fixture-specific production behavior.** Verify general behavior with generated inputs and keep fixed regressions where source spelling or layout is part of the contract.
11. **PROHIBITION 11: No Hardcoded User-Facing Text or Ad-Hoc Bilingual Conditionals:** Every string displayed to the end user (hover markdown tooltips, diagnostics, code action titles, document link titles, notifications) must be externalized in `server/src/i18n/locales/en.json` and `server/src/i18n/locales/es.json` and resolved via `angel_lsp::i18n::FormatMessage(i18n, key, fallback, ...)` (or `bundle.l10n.json` via `l10n.t()` in the client). Hardcoding raw user-facing natural language strings directly in C++ feature handlers or writing ad-hoc language branching (`if (locale == "es")`) is strictly prohibited.
12. **PROHIBITION 12: No Unbounded Parent-Climb Loops or Hot-Path `ts_node_parent` Iteration:** Never write `while (!ts_node_is_null(ts_node_parent(node)))` or unbounded climb loops. Always obtain the root node via `parser::GetRootNode(node)` in $O(1)$ time. In hot-path collectors (`LocalScopeCollector`, `SymbolCollector`), upward parent climbing is strictly prohibited; rely on downward query cursor matching, container stacks, and pre-resolved `TSSymbol` IDs.
13. **PROHIBITION 13: No Hardcoded Predefined Type Names or Blind Namespace Matching in Layer 2:** Never hardcode predefined or add-on type names (`"weakref"`, `"const_weakref"`, `"string"`, `"dictionary"`) in conversion rules, overload rankings, or type checkers (`TypeConversionChecker.cpp`, `OverloadTypeConversions.cpp`, `ConversionRankingEngine.cpp`, `CallChecker.cpp`). All type capabilities (constructors, conversion operators) must be resolved dynamically via `SymbolTable` and template parameter substitution (`BindTemplateArguments`, `SubstituteTemplateParameters`). Furthermore, never use blind `LastScopeSegment(a) == LastScopeSegment(b)` comparisons across different scopes when both types contain explicit namespace qualifiers (`::`).
14. **PROHIBITION 14: No Unnamed `namespace {}`, No `using namespace`, No ODR-Unsafe `.cpp` Structs, No Mechanical Doxygen Slop:** Never introduce anonymous namespaces `namespace {}` or `using namespace` directives (or `using <ns>::<symbol>` in implementation files). Place shared structs in their canonical header and local helper structs in private class scope, as enforced by the AST verifier. Doxygen documentation must provide substantive, informative descriptions; mechanical templates (`/// <brief>` followed by regurgitated type and param names) are strictly forbidden.
15. **PROHIBITION 15: Zero Forward Declarations and Zero Ghost Type Aliases:** Never write standalone struct or class forward declarations (`struct Foo;`, `class Bar;`) in `server/src/`; always include the canonical header. Redundant type aliases aliasing canonical structs within the same namespace (e.g. `using FooRequest = AstCheckContext;`) are strictly prohibited.

---

## 9. Code Quality & Static Analysis Standards

When writing or refactoring C++ code in `server/src/`, all contributions MUST adhere to:
1. **Zero-Duplication Policy (jscpd <= 3%):**
   - Do not duplicate logic across checkers, rules, or feature handlers.
   - Abstract shared logic into Layer 1 utilities or Layer 2 analysis helpers.
2. **Cohesive Design & Readability Standards:**
   - Lizard limits are permanently eliminated.
   - Code readability is maintained via early returns, structured switch-dispatch, and decomposed subroutines only where domain-appropriate.
   - Avoid unnecessary nesting deeper than 3 levels; prioritize clean semantics over mechanical line counting.
3. **Cppcheck Hygiene:**
   - Zero uninitialized variables, null pointer dereferences, resource leaks, or scope-escape defects.
4. **Mandatory Execution Gate:**
   - Always run `python server/scripts/check-quality.py` before committing.
   - If any gate fails, inspect the report, fix root causes, and re-verify.
5. **Compulsory Build Verification Pre-Commit Invariant (Golden Workflow):**
   - Never commit or push changes after running `format-code.py` or Python refactoring scripts without running a full compilation (`cmake --build build --config Release`) and verifying 0 compilation errors and 0 warnings under `/W4 /WX /we4100`.
6. **Mandatory Refactor Ledger & Redundancy Audit Gate:**
   - All struct unifications, function consolidations, flag reparsing eliminations, and layout compactions must be logged in `docs/REFACTOR_LEDGER.json` and validated via `python server/scripts/check-refactor-ledger.py --check-diff develop` and `python server/scripts/audit-codebase-redundancy.py`.

---

## 10. Mandatory Internationalization (i18n/l10n) Standard

AngelLSP supports multi-language environments out of the box (English `en` and Spanish `es`).
All contributions must strictly adhere to:
1. **Zero Hardcoded User-Facing Strings:**
   - Hover explanations, markdown sections, diagnostic descriptions, and code actions must never contain hardcoded English or Spanish prose in Layer 2 or Layer 3.
   - Use `angel_lsp::i18n::FormatMessage(i18n, key, fallbackPattern, ...)` for all dynamic text formatting.
2. **Dual-Language Dictionary Parity:**
   - Add server message keys to both JSON catalogs with matching format fields. CMake validates the catalogs and generates the static message tables in the build directory.
   - Client-side strings in `client/l10n/bundle.l10n.json` must be 1:1 synchronized with `client/l10n/bundle.l10n.es.json`.
3. **No Ad-Hoc Language Conditionals:**
   - Never write `if (locale == "es")` or ternary strings `locale == "es" ? "..." : "..."` in feature handlers.
   - All locale resolution is performed centrally by `angel_lsp::i18n::I18n`.

---

## 11. Mandatory Release, Changelog & Repository Hygiene Standard

Every contribution, version release, and documentation artifact must strictly adhere to:
1. **Zero Undocumented Releases:**
   - Every released tag (e.g. `v1.1.6-exp.2`) must have an exact corresponding version section in `CHANGELOG.md` (and mirrored in `client/CHANGELOG.md` via `server/scripts/sync-changelogs.py`).
2. **Dynamic GitHub Release Body Formatting:**
   - GitHub Releases must never use blank descriptions or purely generic commit summaries (`--generate-notes` fallback only).
   - Every GitHub Release body must contain the exact, detailed, markdown changelog extracted from `CHANGELOG.md` for that release version via `server/scripts/extract-release-notes.py`, preserving the git comparison link.
   - Historical releases on GitHub are synchronized using `server/scripts/sync-github-releases.py`.
3. **Zero Sensitive Data or Hardcoded Local Paths:**
   - Never commit or publish personal email addresses, local developer usernames, or machine-specific drive paths (`C:\Users\...`, `E:\Github\...`) in changelogs, test fixtures, or code.
   - Always use generic paths or dynamic randomized fixtures (`angel_lsp::test::GenerateRandomSymbolName()`).
4. **Canonical Repository Organization & Documentation Hygiene:**
   - Active project documentation and progress tracking are governed SOLELY by `AGENTS.md` and `docs/REFACTOR_STEPS.json`.
   - Obsolete and historical plans are archived in `docs/_legacy_archive/` (`STEPS.json`, `STEPS.md`, `REDUNDANCY_REPORT.json`) and must be strictly ignored by all agents.
   - Active ledger for structural refactoring resides in `docs/REFACTOR_LEDGER.json`.
   - Quality verification scripts reside in `server/scripts/` (`run-harness.ps1`, `check-quality.py`, `verify_pass2_invariants.py`, etc.).
   - AST analysis rules and configuration reside in `server/rules/` (`server/rules/sgconfig.yml`, `server/rules/ast-grep/`).
   - Root directory must remain clean and free of temporary logs, scratch scripts, or ad-hoc config files.

---

## 12. Orchestration Protocol (Planner + Single Persistent Warm Session of `agy` `gemini-3.8-high`)

1. **Role Division:** The Planner directs a single persistent, warm session of `agy` (`gemini-3.8-high`) to perform codebase research, apply code modifications, and audit git diffs.
2. **Token Efficiency:** The Planner delegates AST and symbol lookups to the warm session using MCP tools (`clangd`, `tree-sitter`, `codebase-memory`), requesting concise summaries (~30 lines) containing: `ts_*` calls, `std::string` usages, recursions, and exact call sites.
3. **Surgical Micro-Plans:** Every modification is preceded by an exact contract defining First Pass data structures and `std::string_view` overloads.
4. **Hot Gates & Diff Auditing:** After code edits, the warm session builds in Release, executes tests/harnesses, runs `verify_pass2_invariants.py --strict-string-view` and `check-quality.py`, and inspects `git diff` to ensure zero `#include "parser/ASTUtils.h"`, zero `->Root()`, zero unnecessary `std::string` allocations, and zero code duplication.
