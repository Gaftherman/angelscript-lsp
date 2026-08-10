# Project Overview: AngelScript Language Server (AngelLSP)

AngelLSP is a high-performance C++20 Language Server Protocol (LSP) implementation for the AngelScript programming language (`.as` files), using Tree-Sitter for AST parsing and symbol table analysis.

## Core Architecture
- **Layer 4**: LSP Orchestrator & Server (`server/src/lsp/`, `server/src/main.cpp`)
- **Layer 3**: Feature Handlers (`server/src/features/`) - hover, definition, completion, semantic tokens, signature help.
- **Layer 2**: Analysis & Symbol Management (`server/src/analysis/`) - `SymbolTable`, `SymbolCollector`, `SymbolResolver`.
- **Layer 1**: Core, Document, Parser & Utilities (`server/src/document/`, `server/src/parser/`, `server/src/utils/`).

## Key Directories
- `server/`: C++20 LSP backend server (`CMakeLists.txt`, `src/`, `include/`).
- `client/`: VS Code extension TypeScript client.
- `tree-sitter-angelscript/`: Tree-Sitter grammar and parser for AngelScript.
- `Testing/`: Unit & integration tests using doctest.

## Common Developer Workflows
- **Build C++ Backend Server:**
  ```powershell
  cmake -B server/build -S server -DCMAKE_BUILD_TYPE=Debug
  cmake --build server/build --config Debug
  ```
- **Run Unit & Integration Tests:**
  ```powershell
  cd server/build
  ctest -C Debug --output-on-failure
  ```
- **Build VS Code Extension Client:**
  ```powershell
  cd client
  npm install
  npm run compile
  ```

## Advanced Hybrid Architecture: Kimi K3 (Director) + Gemini `agy` (Executor)

### Maximum Efficiency Flags for `agy`:
- `--dangerously-skip-permissions`: Allows `agy` to modify files and execute scripts non-interactively without prompting for permissions.
- `--mode accept-edits`: Automatically accepts code modifications made by Gemini.
- `--output-format json`: Returns the response in structured JSON format for Kimi to parse the result accurately.

### Standard Execution Command for `agy`:
```powershell
agy -p "<prompt>" --dangerously-skip-permissions --mode accept-edits
```

---

### Work Protocol & Mandatory Verification:

1. **Delegation to Gemini (`agy`):**
   Kimi K3 sends heavy tasks to `agy` to conserve OpenRouter tokens:
   `agy -p "Modify <file> to apply <change>" --dangerously-skip-permissions`

2. **Quality Verification (Quality Control by Kimi K3):**
   Immediately after `agy` finishes, Kimi K3 MUST verify the completed work using `git diff` or by reviewing the file:
   `git diff <file>`
   - Kimi K3 analyzes the `diff` produced by Gemini.
   - If any logic or syntax error is detected in Gemini's edits, Kimi K3 applies fine-tuning adjustments.

3. **Build & Test Verification:**
   Kimi K3 runs the compilation and tests to guarantee 100% functionality:
   `cmake --build server/build --config Debug`
   `ctest --test-dir server/build -C Debug --output-on-failure`

