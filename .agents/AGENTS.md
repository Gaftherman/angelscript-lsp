# Standard Operating Procedure (SOP) & Engineering Standard - AngelLSP

AngelLSP is a high-performance C++20 Language Server Protocol (LSP) implementation for the AngelScript language (`.as`), powered by Tree-Sitter for AST parsing, semantic token resolution, and symbol analysis.

---

## 1. Architectural Layers & Include Matrix

Layer isolation is strict and enforced by `server/scripts/check-layer-includes.py`. Downward-to-upward or cross-feature inclusions are strictly forbidden:

| Layer | Path | Allowed to `#include` | Strictly FORBIDDEN to `#include` |
| :--- | :--- | :--- | :--- |
| **Layer 1: Core / Config** | `core/`, `config/`, `document/`, `parser/`, `utils/` | Own layer, standard C++ libraries | Layers 2, 3, and 4 |
| **Layer 2: Analysis** | `analysis/` | Layer 1, standard C++ libraries | Layers 3 and 4 |
| **Layer 3: Features** | `features/<feature_name>/` | Layers 1 and 2 | Sibling features, Layer 4 |
| **Layer 4: Server / LSP** | `lsp/`, `main.cpp` | Layers 1, 2, and 3 | None (topmost layer) |

---

## 2. Doxygen Documentation Standard (Mandatory for Public APIs)

All classes, structs, member variables, functions, and feature contracts must be documented in **English** using Javadoc-style Doxygen blocks (`/** ... */`).

### Rules:
1. `@brief`: Clear single-line description ending with a period.
2. `@param[in]`, `@param[out]`, or `@param[in,out]`: Explicit direction tag for every parameter, followed by parameter name and purpose.
3. `@return`: Explicit description of return value, including `std::nullopt` or empty-state semantics.
4. `@note` / `@warning`: Concurrency guarantees, thread safety, or AST node lifetime constraints.
5. Never use single-line comments (`//`) to document public interface contracts.

### Canonical Example:
```cpp
namespace lsp::features
{
    /**
     * @brief Immutable context bundle required to compute hover tooltips.
     */
    struct HoverRequest
    {
        const Document& document;          /**< Read-only snapshot of the active script document. */
        const SymbolTable& symbolTable;    /**< Precomputed semantic symbol table for the active scope. */
        Position position;                 /**< UTF-16 character position where hover was triggered. */
    };

    /**
     * @brief Resolves hover tooltip information for an AST node at a given document coordinate.
     * @param[in] request Immutable context payload containing document, symbols, and coordinates.
     * @return An optional HoverResult containing Markdown contents; std::nullopt if the position
     *         does not correspond to a resolvable symbol or comment.
     * @note Thread-safe. Operates purely on const references without mutating shared or global state.
     */
    std::optional<HoverResult> GetHover(const HoverRequest& request);
}
```

---

## 3. Conventional Commits & Git Hygiene Standard

Every commit must adhere strictly to the Conventional Commits specification. Unstructured commits or commits containing temporary debugging code are rejected.

### Format:
```
<type>(<scope>): <short imperative description>

[optional body explaining context, edge cases, and rationale]

[optional footer(s): Closes #123, Breaking-Change: ...]
```

### Allowed Types:
- `feat`: A new user-facing LSP capability or feature handler.
- `fix`: A bug fix in parser, analysis, scheduler, or diagnostics.
- `test`: Adding missing tests, eliminating flakiness, or refactoring test harnesses.
- `perf`: Performance optimizations reducing latency or memory footprint.
- `refactor`: Code restructurings without functional changes.
- `style`: Formatting, Allman brace adjustments, or whitespace.
- `docs`: Documentation only changes (Doxygen, README, AGENTS.md).
- `chore`: CMake adjustments, CI workflows, script improvements, or gitignore updates.

### Allowed Scopes:
- `core`, `parser`, `analysis`, `features`, `server`, `harness`, `tests`, `docs`.

### Quality Rules:
- Commits must be **atomic** (one logical change per commit).
- **Zero debug code**: Never commit `std::cout`, `printf`, or temporary tracing logs.

---

## 4. Deterministic Testing & Performance Budget SLA

1. **Deterministic Schedulers:** No `sleep_for` in tests. Background analysis must be synchronized deterministically using `AnalysisScheduler::DrainQueue()`.
2. **In-Memory Testing:** Feature tests must use `TestUtils.h` (`CreateTestDocument`, `PopulateTestSymbolTable`) without filesystem I/O.
3. **Performance SLA:**
   - `Hover` / `Definition`: < 20 ms.
   - `Completion`: < 50 ms.
   - `SemanticTokens`: < 80 ms per 1,000 lines.
   - Complete CTest suite run: < 120s across all 1,771+ test cases with zero flakiness.

---

## 5. Tooling & Static Analysis Standards

- **Code Style:** Strict Allman style with 4-space indentation enforced by `.clang-format`.
- **Static Analysis:** Audited by `clang-tidy` (`.clang-tidy`) and `cppcheck` via `run-harness.ps1 -FullAudit`.
- **AST & Semantic Exploration:** Handled via `@nendo/tree-sitter-mcp` and `@felipeerias/clangd-mcp-server`.
