# Standard Operating Procedure (SOP) & Engineering Standard - AngelLSP

AngelLSP is a high-performance C++20 Language Server Protocol (LSP) implementation for the AngelScript language (`.as`), powered by Tree-Sitter for AST parsing, semantic token resolution, and symbol analysis.

### How to read this document
- **§1-§2 Structure:** layers, include matrix and concurrency invariants (what may depend on what).
- **§3-§5 Code contracts:** signatures, tree-sitter traversal, Doxygen (how code inside a layer is written).
- **§6-§7 Process:** commits and testing.
- **§8 Prohibitions:** the absolute bans (16). Each one has a stable number cited by scripts, the ledger and changelogs; never renumber.
- **§9 Gates:** the executable checks that enforce the rules above. A rule without a gate is a review obligation.
- **§10-§11 Product hygiene:** i18n, releases, repository layout.
- **§12 Orchestration:** how the Planner and the `agy` executor split work, and when a performance change is kept.
- Rule identifiers (`Rule 1.2`, `4.7`-`4.11`, `PROHIBITION n`) are stable anchors. Add new rules at the end of their section; deprecate by editing the text, not by deleting the number.

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
- **Sub-Layer 2.1 (Index):** `analysis/index/` - The first pass: AST indexing, lexical structure and immutable facts. Core types: `NodeIndex`, `SymbolTable`, `SymbolCollector`, `ScopeTree`, `LocalScopeCollector`, `DeclarationRecovery`, `CallGraph`, `SemanticNodes`, `TypeExtraction`, `RuleIndex`, `OperatorSiteIndex`, `SpanFactSearch`. Everything named `*Facts` (`CallSiteFacts`, `ValueExpressionFacts`, `MemberAccessFacts`, `IsolationSiteFacts`, `IsolationIdentifierFacts`, `ControlFlowFacts`, `NullFlowFacts`, `SemanticTokenFacts`, `HoverFacts`, `CompletionFacts`, ...) is an AST-free value struct plus its extractor, consumed by the second pass. The directory listing is the source of truth for the full set.
- **Naming conventions inside `analysis/`:** `*Facts` = first-pass extracted data; `*Internal.h` = private to its directory, never included from another sub-layer; `*Common.h` = helpers shared by one checker family; `<Name><Aspect>.cpp` = one split of a unit that would exceed 300 lines (Rule 4.5).
- **Sub-Layer 2.2 (Types):** `analysis/types/` - Type representation (`TypeSanitization`, `ExpressionTypeCache`, `EngineConfigConverter`, `TypeConversionValues`).
- **Sub-Layer 2.3 (Diagnostics):** `analysis/diagnostics/` - Diagnostic error codes and context (`Diagnostics`, `DiagnosticCodes`, `DiagnosticContext`, `DiagnosticSuppression`).
- **Sub-Layer 2.4 (Documentation):** `analysis/documentation/` - Doc comment parsing and markdown formatting (`DocComment`, `DoxygenMarkdown`, `SignatureFormatter`).
- **Sub-Layer 2.5 (Resolution):** `analysis/resolution/` - Semantic target, type and overload resolution (`SemanticHelpers`, `TargetResolution`, `OverloadResolver`, `AmbiguityDetector`, `ConversionRankingEngine`, `OverloadTypeConversions`, `OperandRangeResolution`, `ClassHierarchyResolver`, `TemplateTypeResolver`, `MixinInclusion`, `ExpressionType*`, `ReferenceCounting*`).
- **Sub-Layer 2.6a (Rules):** `analysis/rules/` - Semantic language rules (`ClassRules`, `EngineRuleChecker`, `FunctionRules` [`FunctionSignatureRules`, `FunctionDeclarationRules`, `FunctionParameterRules`], `OperatorRules`, `TypeRules` [`TypeEnumRules`, `TypeDuplicateRules`], `UnusedSymbolChecker` [`UnusedReferenceCollection`, `UnusedEngineExemptions`, `UnusedDefinitionChecks`], `VariableRules` [`VariableTypeRules`, `VariableLocalRules`]).
- **Sub-Layer 2.6b (Checkers):** `analysis/checkers/` - Semantic verification checkers (`AccessChecker`, `BinaryOperatorChecker`, `BinaryOperatorHelpers`, `CallChecker`, `ComparisonOperatorChecker`, `ConstChecker`, `ControlFlowChecker`, `DefiniteAssignmentChecker`, `HandleComparisonChecker`, `InitializerListChecker`, `IsolationChecker`, `LValueChecker`, `NamespaceChecker`, `RepeatedConversionChecker`, `TypeConversionChecker`, and `null_safety/` [`NullSafetyTypes`, `NullSafetyChecker`, `NullSafetyCondition`, `NullSafetyDecl`, `NullSafetyExpr`, `NullSafetyParam`]).
- **Sub-Layer 2.7 (Mediator & Coordinator):** `analysis/mediator/` (`SemanticMediator`) and `analysis/` root - Semantic coordinator facade (`SemanticAnalyzer`, `SemanticAnalysisRequest`).
- **Sub-Layer 2.8 (Formatting):** `analysis/formatting/` - Lexer-based source formatter and `.clang-format` style reading (`FormattingEngine`, `FormattingTokens`, `FormattingLexer`, `FormattingLineBuilder`, `FormattingSpacing`, `FormattingRenderer`, `ClangFormatReader`, `ClangFormatPresets`, `PredefinedStubFormatter`). Protocol-free: options arrive as `FormatLayoutOptions`.
- **Sub-Layer 2.9 (Completion):** `analysis/completion/` - Completion context detection, candidate collection and ranking (`CompletionEngine`, `CompletionTypes`). Protocol-free: results are `CompletionEntry` values that `features/completion` maps to LSP items; syntax arrives through `analysis/index/CompletionFacts` and `analysis/resolution/CompletionReceiverResolution`.
- **Sub-Layer 2.10 (Code Action):** `analysis/code_action/` - Code action and refactoring providers (`CodeActionEngine`, `CodeActionTypes`). Protocol-free: results are `CodeActionEntry` values that `features/code_action` maps to LSP actions; syntax arrives through the `analysis/index/*Facts` headers.
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

| Term | Meaning in this document |
| :--- | :--- |
| **First pass** | `server/src/parser/` and `server/src/analysis/index/`: the only code that may use `ts_*`, `TSNode` and `parser/ASTUtils.h`. It runs once per syntax revision and produces immutable facts. |
| **Second pass** | `features/`, `analysis/{resolution,checkers,rules,completion,code_action}/`, `lsp/` and `utils/`: consumes facts, with 0 `ts_*` calls. |
| **Hot path** | Code that runs per node or per identifier of a whole document (collectors, query passes, token classification) or per request item (code lens targets, references). |
| **Fact** | A first-pass value struct with spans and ids, free of AST pointers, stored in `NodeIndex`. |

1. **Structure extraction:** Reuse the first-pass cursor or its index for document-wide work. Keep precompiled S-expression queries for targeted extraction and collectors awaiting migration. Avoid nested index-based `ts_node_child` loops.
2. **Flat Cursor Traversal:** When full sub-tree iteration is necessary, use flat `TSTreeCursor` loops (`ts_tree_cursor_goto_first_child` / `ts_tree_cursor_goto_next_sibling`) without recursion on the C++ execution stack.
3. **Index-First Feature Queries:** Feature handlers (Layer 3) obtain syntax only through Layer 2 indexed facts and analysis APIs, never from raw `TSNode`s (Rule 4.10). Ad-hoc text lexers (`ConsumeIdentifier`, manual string slicing) are strictly forbidden in Layer 3 (PROHIBITION 5).
4. **Cohesive C++ Design:** Lizard is permanently removed: no cyclomatic or per-function line limits. Functions stay cohesive and readable without fragmentation into micro-functions or synthetic parameter bags (PROHIBITION 8). Use domain structs, overloads and interfaces, with Doxygen (§5). The only size limits are the 4-parameter ceiling (§3.1) and the 300-line file target (4.5).
5. **Anti-Monolith & 300-Line Limit:** Production source files (`.cpp`) must target $\le 300$ net lines of code. Monolithic files exceeding this ceiling must be decomposed into cohesive, single-responsibility sub-units.
6. **No Root-Cause Bypass:** Never patch edge cases using string comparison hardcoding (`if (name == "...")`). Fixes must reside in grammar queries, symbol resolution passes, or type rules. See Prohibition 16 for type names, keywords and literals that already have an enum or predicate.
7. **AST Traversal & Parent-Climb Invariants (see PROHIBITION 12):**
    - Root retrieval is always `parser::GetRootNode(node)` ($O(1)$); never loop `ts_node_parent` to the root.
    - Iterative `ts_node_parent` climbs and `ts_node_type` string comparisons are forbidden in hot-path collectors and query passes (`LocalScopeCollector`, `SymbolCollector`, token classification). Use a container stack, byte-range containment, pre-cached `TSSymbol` ids or a bounded hop count (`depth < 16`).
    - Cost model: `ts_node_parent` is $O(\text{depth} \times \text{children per level})$, so a climb inside a long sibling list is linear in the sibling count. Per-node climbs over a whole document are the expensive pattern; replace them with one flat cursor pass that carries the ancestor context on a stack (`IsolationIdentifierFacts` is the reference implementation).
    - A rewrite of this kind is kept only if it passes the measurement rule of §12.4; equivalence alone is not a reason to merge it.
8. **Official Layer Architecture Tool Matrix:**
   - Use `ScopeTree::FindInnermostScope(byte)` for local definitions and the lexical records in `NodeIndex` for container and namespace import lookup. The positional lookup uses binary search followed by enclosing links. Its cost includes the scopes visited and the names returned. A namespace import applies within its owning lexical scope.
   - **AST Navigation & Parent Isolation:** Upward traversal must strictly use `parser::GetParent(node)` (which safely verifies `!ts_node_is_null`). Calling raw `ts_node_parent` is strictly forbidden outside `server/src/parser/ASTUtils.h`. Root retrieval must use `parser::GetRootNode(node)` in $O(1)$.
   - **Node Text Access:** `parser::NodeText(node, source)` is the single text accessor and returns a `std::string_view`; copy it explicitly with `std::string(...)` only when the text must outlive the source buffer. Operator tokens use `parser::OperatorSpelling(node)`.
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
      - `std::string_view` for string inspection, parameters, and paths. Prohibited: `const std::string&` parameters that are only read, lookups through a temporary `std::string(...)` (use transparent hashing from `utils/TransparentHash.h`), and copies made only to inspect. Owning storage (members, containers, returned values, sinks that keep the value) is allowed. Enforced via `server/scripts/verify_pass2_invariants.py --strict-string-view`.
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
- **Scopes:** `core`, `parser`, `analysis`, `index`, `features`, `server`, `client`, `harness`, `scripts`, `tests`, `docs`.
- Subject in the imperative mood, no trailing period. Group one coherent change per commit (one orchestration job per commit, §12).
- Attribution trailers requested by the host environment go at the end of the message body.
- Zero debug code: Never commit `std::cout`, `printf`, or temporary tracing logs.

---

## 7. Invariant-Based Testing & Anti-Overfitting Governance

1. **Generalizable fixtures:** Use generated identifiers and paths to exercise semantic invariants. Use fixed fixtures for precise regressions or external corpus parity, and record seeds when generated structure affects reproduction.
2. **Path Containment Invariant:** Path resolution must be tested using dynamic temporary sandboxes with multi-depth randomized traversal sequences. Testing only fixed strings like `../../etc/passwd` is strictly prohibited.
3. **Transport Security Invariant:** JSON-RPC transport layers must enforce a bounded envelope (`MAX_LSP_PAYLOAD_SIZE = 16 MB`). Payloads outside bounds or with malformed headers must be rejected without allocations or crashes.
4. **Topological Graph Invariant:** Invalidation traversals over `WorkspaceIncludeGraph` must be tested on randomized DAGs asserting strictly $O(V + E)$ deduplicated visits.
5. **Equivalence for Refactors:** A change that must not alter behavior is validated by (a) the full suite, (b) the differential corpus run against the AngelScript oracle (`server/scripts/differential-validate.py`; expected: 0 discrepancies, 0 oracle contradictions) and, when it replaces an algorithm, (c) a test that compares the new implementation with a copy of the old one on generated inputs (record the seed).
6. **Struct Layout:** Size expectations for fact and index structs live in `FactLayoutSizeTest.cpp`, not in scattered `static_assert`s.
7. **Benchmarks:** Benchmark cases are `doctest::skip` and run with `-ns`. They report timings only; they never assert on elapsed time (see the ban above). Memory conclusions use process max RSS, not the benchmark's RSS delta, which is noise.

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

## 8. The 16 Absolute Prohibitions

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
12. **PROHIBITION 12: No Unbounded Parent-Climb Loops or Hot-Path `ts_node_parent` Iteration:** Never write `while (!ts_node_is_null(ts_node_parent(node)))` or unbounded climb loops; obtain the root with `parser::GetRootNode(node)`. In hot-path collectors (`LocalScopeCollector`, `SymbolCollector`) upward climbing is prohibited; rely on downward query matching, container stacks and pre-resolved `TSSymbol` ids. Details and cost model: Rule 4.7.
13. **PROHIBITION 13: No Hardcoded Predefined Type Names or Blind Namespace Matching in Layer 2:** Never hardcode predefined or add-on type names (`"weakref"`, `"const_weakref"`, `"string"`, `"dictionary"`) in conversion rules, overload rankings, or type checkers (`TypeConversionChecker.cpp`, `OverloadTypeConversions.cpp`, `ConversionRankingEngine.cpp`, `CallChecker.cpp`). All type capabilities (constructors, conversion operators) must be resolved dynamically via `SymbolTable` and template parameter substitution (`BindTemplateArguments`, `SubstituteTemplateParameters`). Furthermore, never use blind `LastScopeSegment(a) == LastScopeSegment(b)` comparisons across different scopes when both types contain explicit namespace qualifiers (`::`).
14. **PROHIBITION 14: No Unnamed `namespace {}`, No `using namespace`, No ODR-Unsafe `.cpp` Structs, No Mechanical Doxygen Slop:** Never introduce anonymous namespaces `namespace {}` or `using namespace` directives (or `using <ns>::<symbol>` in implementation files). Place shared structs in their canonical header and local helper structs in private class scope, as enforced by the AST verifier. Doxygen documentation must provide substantive, informative descriptions; mechanical templates (`/// <brief>` followed by regurgitated type and param names) are strictly forbidden.
15. **PROHIBITION 15: Zero Forward Declarations and Zero Ghost Type Aliases:** Never write standalone struct or class forward declarations (`struct Foo;`, `class Bar;`) in `server/src/`; always include the canonical header. Redundant type aliases aliasing canonical structs within the same namespace (e.g. `using FooRequest = AstCheckContext;`) are strictly prohibited.

16. **PROHIBITION 16: No Raw Literal Comparisons When a Canonical Form Exists:** Layer 2-4 code must not compare type names, keywords or literals as raw strings (`== "auto"`, `!= "void"`, `== "null"`, `== "true"`, `find('<')`, `starts_with("const")`) when the first pass or a shared helper already classifies them. Use `TypeKind` (from `ParameterInformation`, `Symbol` or the expression facts), `parser::keywords::*`, `GrammarSymbols`, the `TypeSanitization` predicates (`HasLeadingConst`, `IsHandleTypeView`, ...) and `ParseTemplateType`. If no canonical form exists, add one predicate and migrate every caller. Parsing external text (`.clang-format`, DAP and JSON values) is exempt. Enforced as a ratchet by `server/scripts/check-raw-literal-compare.py` against `server/rules/raw-literal-baseline.json`: per-file counts may only go down.

---

## 9. Code Quality & Static Analysis Standards

When writing or refactoring C++ code in `server/src/`, all contributions MUST adhere to:
1. **Zero-Duplication Policy (jscpd <= 3%):**
   - Do not duplicate logic across checkers, rules, or feature handlers.
   - Abstract shared logic into Layer 1 utilities or Layer 2 analysis helpers.
2. **Cohesive Design & Readability Standards:**
   - Rule 4.4 governs function size. Prefer early returns, structured switch-dispatch and decomposition only where it is domain-appropriate.
   - Avoid nesting deeper than 3 levels; prioritize clear semantics over mechanical line counting.
3. **Cppcheck Hygiene:**
   - Zero uninitialized variables, null pointer dereferences, resource leaks, or scope-escape defects.
4. **Mandatory Execution Gate:**
   - Always run `python server/scripts/check-quality.py` before committing.
   - If any gate fails, inspect the report, fix root causes, and re-verify.
5. **Compulsory Build Verification Pre-Commit Invariant (Golden Workflow):**
   - Never commit or push changes after running `format-code.py` or Python refactoring scripts without running a full compilation (`cmake --build build --config Release`) and verifying 0 compilation errors and 0 warnings under `/W4 /WX /we4100`.
6. **Mandatory Refactor Ledger & Redundancy Audit Gate:**
   - All struct unifications, function consolidations, flag reparsing eliminations, and layout compactions must be logged in `docs/REFACTOR_LEDGER.json` and validated via `python server/scripts/check-refactor-ledger.py --check-diff develop` and `python server/scripts/audit-codebase-redundancy.py`.
7. **Gate Map (what `check-quality.py` runs, and which rule each gate enforces):**

| Gate (script) | Enforces |
| :--- | :--- |
| `check-layer-includes.py` | §1 include matrix, Rule 1.2 |
| `check-clean-signatures.py` | §3, PROHIBITION 9 |
| `verify_pass2_invariants.py --strict-string-view` | Rule 4.10 (second pass: 0 `ts_*`, `std::string_view`) |
| `check-ast-traversal.py` | Rules 4.1-4.2, 4.7, PROHIBITIONS 6, 7, 12, 14, 15 |
| `check-raw-literal-compare.py` | PROHIBITION 16 (ratchet against `server/rules/raw-literal-baseline.json`) |
| `check-grammar-names.py`, `check-grammar-pin.py` | Rule 4.8 (`GrammarSymbols`), grammar pin |
| `check-diagnostic-codes.py` | Diagnostic code registry |
| `check-client-l10n.py`, `sync-readme-settings.py --check` | §10 parity, settings catalog |
| `check-refactor-ledger.py` | §9.6 |
| `ast-grep` scan (`server/rules/`), cppcheck, jscpd, `format-code.py --check` | §9.1, §9.3, PROHIBITION 14, Clang-Format style |

8. **Ratchet Policy:** A ratchet gate compares per-file counts with a committed baseline (`server/rules/raw-literal-baseline.json`). Counts may only go down; lowering a file is followed by the gate's `--update` in the same change, and a baseline is never raised to make a gate pass. A baseline at zero is promoted to a hard gate.
9. **Ledger Ids:** Each orchestration job gets one ledger entry with id `REF-<phase>-<topic>` (for example `REF-P20-ISOLATION-IDENT-PASS`) and a valid action from the existing entries. A reverted job leaves no entry.

   The build target `angel_lsp_anti_slop_gate` runs the AST traversal gate before compilation, so a violation also fails `cmake --build`. When a new rule gets a gate, add it to this table in the same change.

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

## 12. Orchestration Protocol (Planner + `agy` executor)

1. **Role Division:** The Planner designs, briefs, builds, measures and decides. The `agy` executor (model `Gemini 3.8 Flash (High)`, verify the exact name with `agy models`) researches and edits code. `agy` never builds and never runs the Python gates, `ast-grep`, `cppcheck` or `jscpd`; the Planner owns every verification step. Nobody except the user commits or pushes.
2. **Briefs and Jobs:** One topic per job, one write job at a time on the working tree, and no Planner edits while a job runs. Each job has a self-contained brief file kept outside the repository that states: the measurement or motivation, the cause, the exact task and its files, the hard rules (§3-§4, §8) and the expected report. The prompt passed to `agy` carries only repo-relative names and a pointer to the brief, with stdin closed. Allow twice the measured duration as the timeout (8 to 20 minutes is typical). A job is finished when the `agy.exe` process has exited and its report is read, not when the launcher returns.
3. **Planner Closing Gates (per job):**
   - Release build on the target platform: 0 errors, 0 warnings under `/W4 /WX /we4100`.
   - Full test suite, then `format-code.py`, a rebuild, `check-quality.py` and `verify_pass2_invariants.py --strict-string-view`.
   - Differential corpus: 0 discrepancies (Testing Invariant 5).
   - Ledger entry (`docs/REFACTOR_LEDGER.json`, id `REF-<phase>-<topic>`) and a read of the full `git diff` for stray `#include "parser/ASTUtils.h"`, `->Root()`, avoidable `std::string` allocations and duplication.
4. **Performance Acceptance Rule:** A change made for speed is kept only if its target benchmark row improves by at least 5% at both 5k and 20k lines **and** no other row worsens by more than about 3% (noise floor). Otherwise revert every file of the job, including its tests and ledger entry, and report the numbers. Measure on Linux with `perf` in the container, comparing a baseline binary built from `git stash` against the new binary in interleaved runs (4 per side; 6 for noisy rows such as `codeLens`), and compare medians of p50 and p95. Absolute numbers from different sessions are not comparable. If profiling shows the remaining cost is spread over the parser and index build, report that limit instead of forcing the target.
5. **Surgical Micro-Plans and Token Efficiency:** Every modification starts from an exact contract (first-pass data structures, `std::string_view` overloads). Delegate AST and symbol lookups to `agy` and ask for short summaries (~30 lines): `ts_*` calls, `std::string` usages, recursions, exact call sites.
6. **Privacy:** Briefs, prompts, ledger entries, fixtures and changelogs never contain personal emails, usernames or machine paths, and nothing personal goes into requests to external services.
