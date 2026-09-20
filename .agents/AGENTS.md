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

---

## 2. Function Signature & Parameter Governance

1. **Parameter Ceiling:** Functions must accept at most 4 parameters. If more inputs are needed, bundle them into an immutable struct (e.g., `struct <Feature>Request`).
2. **Zero Dead Parameters (`/we4100`):** Unreferenced formal parameters are prohibited. If an argument is no longer needed, delete it from the header, implementation, and all call sites immediately.
3. **No Unnamed or Commented-Out Parameters:** Never bypass warnings using `int /* b */` or unnamed parameters (`int`). Leaving dead parameters in call sites is a severe defect.
4. **Virtual Method Overrides:** Only virtual method overrides may silence unused parameters using `[[maybe_unused]]` with an explicit Doxygen `@note`.

---

## 3. Tree-Sitter Traversal & Anti-Monolith Mandate

1. **Query-First Rule:** Structure extraction must use precompiled S-expression queries (`BuiltQueries.h`) and `ts_query_cursor_*`. Never write nested index-based `ts_node_child` loops.
2. **Flat Cursor Traversal:** When full sub-tree iteration is necessary, use flat `TSTreeCursor` loops (`ts_tree_cursor_goto_first_child` / `ts_tree_cursor_goto_next_sibling`) without recursion on the C++ execution stack.
3. **Complexity Ceiling:** Functions must not exceed 15 Cyclomatic Complexity points or 70 net lines of code (enforced by `lizard`).
4. **No Root-Cause Bypass:** Never patch edge cases using string comparison hardcoding (`if (name == "...")`). Fixes must reside in grammar queries, symbol resolution passes, or type rules.

---

## 4. Doxygen Documentation Standard

All public classes, structs, member variables, functions, and feature contracts must be documented in **English** using Javadoc-style blocks (`/** ... */`):
- `@brief`: Concise single-line summary ending with a period.
- `@param[in/out]`: Explicit direction tag, parameter name, and purpose.
- `@return`: Detailed return description, including `std::nullopt` or empty-state semantics.
- `@note` / `@warning`: Concurrency guarantees or AST node lifetime rules.

---

## 5. Conventional Commits & Git Hygiene

Format: `<type>(<scope>): <short imperative description>`
- **Types:** `feat`, `fix`, `test`, `perf`, `refactor`, `style`, `docs`, `chore`.
- **Scopes:** `core`, `parser`, `analysis`, `features`, `server`, `harness`, `tests`, `docs`.
- Zero debug code: Never commit `std::cout`, `printf`, or temporary tracing logs.
