# Standard Operating Procedure (SOP) & Engineering Standard - AngelLSP

AngelLSP is a high-performance C++20 Language Server Protocol (LSP) server for the AngelScript language (`.as`), using Tree-Sitter for AST parsing, semantic token resolution, and symbol analysis.

---

## 1. Architectural Layers & Include Matrix

Circular or downward-to-upward inclusions are strictly prohibited and checked via `server/scripts/check-layer-includes.py`:

| Layer | Path | Allowed to `#include` | FORBIDDEN to `#include` |
| :--- | :--- | :--- | :--- |
| **Layer 1: Core / Config** | `core/`, `config/`, `document/`, `parser/`, `utils/` | Own layer, standard C++ libraries | Layers 2, 3, and 4 |
| **Layer 2: Analysis** | `analysis/` | Layer 1, standard C++ libraries | Layers 3 and 4 |
| **Layer 3: Features** | `features/<feature_name>/` | Layers 1 and 2 | Sibling features, Layer 4 |
| **Layer 4: Server / LSP** | `lsp/`, `main.cpp` | Layers 1, 2, and 3 | None (topmost layer) |

---

## 2. Doxygen Documentation Standard (Mandatory for All Public APIs)

All classes, structs, member variables, functions, and feature contracts must be documented in **English** using Javadoc-style Doxygen comments (`/** ... */`).

### Rules:
1. **`@brief`**: A concise single-line description ending with a period.
2. **`@param[in/out]`**: Explicit direction tag for each parameter, followed by parameter name and purpose.
3. **`@return`**: Describes the returned value and empty/nullopt semantics.
4. **`@note` / `@warning`**: Concurrency constraints, thread-safety guarantees, or AST lifecycle rules.
5. **No inline comments for API contracts**: Do not use `//` comments for function contracts.

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
     * @note Thread-safe. This function operates purely on const references without modifying global state.
     */
    std::optional<HoverResult> GetHover(const HoverRequest& request);
}
```

---

## 3. Conventional Commits & Git Hygiene Standard

Every commit must adhere strictly to the Conventional Commits specification. Unstructured commits or commits containing temporary debugging logs are rejected.

### Format:
```
<type>(<scope>): <short imperative description>

[optional body explaining WHY, context, or edge cases]

[optional footer(s): Closes #123, Breaking-Change: ...]
```

### Allowed Types:
- **`feat`**: A new LSP feature, user-facing capability, or language extension.
- **`fix`**: A bug fix in parsing, type checking, scheduling, or diagnostics.
- **`test`**: Adding missing tests, refactoring test helpers, or improving coverage.
- **`perf`**: A code change that improves throughput or reduces memory/latency.
- **`refactor`**: A code change that neither fixes a bug nor adds a feature.
- **`style`**: Changes that do not affect code logic (clang-format, whitespace, Allman braces).
- **`docs`**: Documentation only changes (Doxygen, README, AGENTS.md).
- **`chore`**: Maintenance tasks, CMake adjustments, CI workflows, or gitignore updates.

### Allowed Scopes:
- `core`, `parser`, `analysis`, `features`, `server`, `harness`, `tests`, `docs`.

### Examples:
- `feat(features): implement signature help handler for overloaded constructors`
- `fix(analysis): resolve false positive in definite assignment loop break`
- `test(harness): replace sleep_for with deterministic DrainQueue barrier`
- `docs(architecture): update layer matrix and Doxygen standards in AGENTS.md`

### Rules:
- Commits must be **atomic** (one logical change per commit).
- **Zero debug code**: Never commit `std::cout`, `printf`, or temporary trace logs.

---

## 4. Deterministic Testing & Performance Budget SLA

1. **Deterministic Execution:** No `sleep_for` in any test. Background tasks must be synchronized using `AnalysisScheduler::DrainQueue()`.
2. **In-Memory Testing:** Feature tests must use `TestUtils.h` (`CreateTestDocument`, `PopulateTestSymbolTable`) without touching physical disk I/O.
3. **Performance SLA:**
   - `Hover` / `Definition`: < 20 ms.
   - `Completion`: < 50 ms.
   - `SemanticTokens`: < 80 ms per 1,000 lines.
   - Full test suite execution target: < 120s across all 1,771 tests.

---

## 5. Tooling & Linting Standards

- **Code Style:** Strict Allman style with 4-space indentation enforced by `.clang-format`.
- **Static Analysis:** `clang-tidy` rules in `.clang-tidy` and `cppcheck` via `run-harness.ps1 -FullAudit`.
- **AST Exploration:** Handled via `@nendo/tree-sitter-mcp` and `@felipeerias/clangd-mcp-server`.
