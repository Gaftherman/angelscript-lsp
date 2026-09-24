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
- **Protocol Isolation:** `analysis/` and `parser/` must compile without the LSP protocol library (`<lsp/...>` or `utils/LspLogger.h`). Loggers must be held by forward declaration in headers (`namespace angel_lsp::utils { class LspLogger; }`).

---

## 2. Concurrency & AST Memory Invariants

1. **No Raw Tree Borrowing:** `DocumentStore` must never return a raw `TSTree*` that outlives its reader lock. Functions accessing ASTs must retain a `std::shared_ptr<const document::Document>` handle or operate on thread-local copies created via `ts_tree_copy()`.
2. **No Uncaught Thread Exceptions:** All secondary threads (`std::thread`, `AnalysisScheduler`, `WorkspaceScan`) must wrap execution in top-level `try/catch` handlers that log fatal errors to disk via `MultiFileLogger` and shut down gracefully without invoking `std::terminate()`.
3. **Mandatory Safe AST Navigation:** Always verify `!ts_node_is_null(node)` before invoking `ts_node_type()`, `ts_node_start_byte()`, or accessing child nodes.
4. **AST Thread Safety Invariant:** `TSTree*` pointers must never be shared across threads without `ts_tree_copy()`. Concurrency suites must validate simultaneous mutation and reading under randomized workloads.

---

## 3. Function Signature & Parameter Governance

1. **Parameter Ceiling:** Functions must accept at most 4 parameters. If more inputs are needed, bundle them into an immutable struct (e.g., `struct <Feature>Request`).
2. **Zero Dead Parameters (`/we4100`):** Unreferenced formal parameters are prohibited. If an argument is no longer needed, delete it from the header, implementation, and all call sites immediately.
3. **No Unnamed or Commented-Out Parameters:** Never bypass warnings using `int /* b */` or unnamed parameters (`int`). Leaving dead parameters in call sites is a severe defect.
4. **Virtual Method Overrides:** Only virtual method overrides may silence unused parameters using `[[maybe_unused]]` with an explicit Doxygen `@note`.

---

## 4. Tree-Sitter Traversal & Anti-Monolith Mandate

1. **Query-First Rule:** Structure extraction must use precompiled S-expression queries (`BuiltQueries.h`) and `ts_query_cursor_*`. Never write nested index-based `ts_node_child` loops.
2. **Flat Cursor Traversal:** When full sub-tree iteration is necessary, use flat `TSTreeCursor` loops (`ts_tree_cursor_goto_first_child` / `ts_tree_cursor_goto_next_sibling`) without recursion on the C++ execution stack.
3. **AST-First Feature Queries:** Feature handlers must query Tree-Sitter AST nodes or Layer 2 analysis APIs. Ad-hoc text lexers (`ConsumeIdentifier`, manual string slicing) are strictly forbidden in Layer 3.
4. **Complexity Ceiling:** Functions must not exceed 15 Cyclomatic Complexity points or 70 net lines of code (enforced by `lizard`).
5. **Anti-Monolith & 300-Line Limit:** Production source files (`.cpp`) must target $\le 300$ net lines of code. Monolithic files exceeding this ceiling must be decomposed into cohesive, single-responsibility sub-units.
6. **No Root-Cause Bypass:** Never patch edge cases using string comparison hardcoding (`if (name == "...")`). Fixes must reside in grammar queries, symbol resolution passes, or type rules.

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

1. **Mandatory Randomization in Test Fixtures:** Tests must never assert against predictable, hardcoded symbol names. Use `angel_lsp::test::GenerateRandomSymbolName()` to construct unique identifiers, types, and file names at runtime.
2. **Path Containment Invariant:** Path resolution must be tested using dynamic temporary sandboxes with multi-depth randomized traversal sequences. Testing only fixed strings like `../../etc/passwd` is strictly prohibited.
3. **Transport Security Invariant:** JSON-RPC transport layers must enforce a bounded envelope (`MAX_LSP_PAYLOAD_SIZE = 16 MB`). Payloads outside bounds or with malformed headers must be rejected without allocations or crashes.
4. **Topological Graph Invariant:** Invalidation traversals over `WorkspaceIncludeGraph` must be tested on randomized DAGs asserting strictly $O(V + E)$ deduplicated visits.

---

## 8. The 10 Absolute Prohibitions

1. **PROHIBITION 1: No Unprotected Raw AST Pointer Escaping:** Never return or store raw `TSTree*` pointers without an enclosing reader lock or owning `std::shared_ptr<const document::Document>`.
2. **PROHIBITION 2: No Uncaught Exceptions Escaping Secondary Threads:** Never allow an exception to escape a `std::thread` boundary; unhandled exceptions that invoke `std::terminate()` are classified as critical severity bugs.
3. **PROHIBITION 3: No Cross-Feature Includes (Layer 3 Isolation):** Never include headers from sibling features in `server/src/features/`. Cross-cutting capabilities must be factored into Layer 2 (`analysis/`).
4. **PROHIBITION 4: No Duplication of Core Analysis Structures:** Never copy and paste core data models (`TargetDescriptor`, `TargetKind`) to circumvent Layer Matrix rules.
5. **PROHIBITION 5: No Ad-Hoc Text Lexers in Layer 3:** Never write custom string slicing lexers (`ConsumeIdentifier`, index-based paren counting) to inspect expressions; always use Tree-Sitter AST nodes or Layer 2 type resolution.
6. **PROHIBITION 6: No Index-Based `ts_node_child` Traversal Loops:** Structure extraction must use precompiled S-expression queries or flat cursor traversal.
7. **PROHIBITION 7: No Stack-Recursive AST Traversals:** Sub-tree traversals must be flat with an explicit depth cap (`k_maxAstDepth = 64`) to prevent stack overflow crashes.
8. **PROHIBITION 8: No Functions Exceeding 15 CCN or 70 Lines:** Every function must pass `lizard -C 15 -L 70 -a 4` without warnings.
9. **PROHIBITION 9: No Dead or Unnamed Parameters (`/we4100`):** Every formal parameter must be named, referenced, or cleanly removed.
10. **PROHIBITION 10: No Static or Hardcoded Test Fixtures:** Tests must use dynamic randomized generators and sandboxes to verify semantic invariants rather than overfitting to fixed strings.
