# Standard Operating Procedure (SOP) & Architecture Guide - AngelLSP

AngelLSP is a high-performance C++20 Language Server Protocol server for the AngelScript language (`.as`), powered by Tree-Sitter for incremental AST parsing, semantic token resolution, and symbol analysis.

---

## 1. Architectural Layers & Include Matrix

Every `#include` must follow this hierarchy. Circular or downward-to-upward inclusions are strictly prohibited:

| Layer | Path | Allowed to `#include` | Strictly FORBIDDEN to `#include` |
| :--- | :--- | :--- | :--- |
| **Layer 1: Core / Config** | `core/`, `config/`, `document/`, `parser/`, `utils/` | Own layer, standard C++ libraries | Layers 2, 3, and 4 |
| **Layer 2: Analysis** | `analysis/` | Layer 1, standard C++ libraries | Layers 3 and 4 |
| **Layer 3: Features** | `features/<feature_name>/` | Layers 1 and 2 | Other sibling features, Layer 4 |
| **Layer 4: Server / LSP** | `lsp/`, `main.cpp` | Layers 1, 2, and 3 | None (topmost layer) |

---

## 2. Feature Implementation Lifecycle (5 Mandatory Steps)

When adding or refactoring an LSP capability (e.g., SignatureHelp, InlayHints):

1. **Pure Functional Contract:** Define immutable inputs (`const &`) in `features/<feature>/<Feature>Handler.h`. Never mutate state or include other feature headers.
2. **Deterministic Implementation:** Implement in `.cpp` returning `std::optional` or empty collections. Never throw exceptions (`throw`) across the LSP bridge.
3. **In-Memory Unit Test:** Create `tests/<Feature>Test.cpp` using `helpers/TestUtils.h` (`CreateTestDocument`, `PopulateTestSymbolTable`) without filesystem I/O.
4. **Capability & Kill-Switch Registration:** Register capability flags in `ServerConfig.h` and wrap LSP handlers in `Server.cpp` using capability guards.
5. **Full Harness Verification:** Execute `powershell -File run-harness.ps1 -CheckFormatting` and confirm 100% passing tests.

---

## 3. Tooling & Semantic Analysis Harness

- **Compilation Database:** Generated via `Ninja Multi-Config` at `server/build/compile_commands.json` and mirrored at project root.
- **AST & Semantic Inspection:** Handled via `@felipeerias/clangd-mcp-server` using `clangd` (LLVM 23+).
- **Style Standard:** Strict Allman braces (`.clang-format`), Doxygen comments in English, no debug logs in commits.
- **Static Analysis:** Audited by `clang-tidy` (integrated into `clangd`) and `cppcheck`.
