# Standard Operating Procedure (SOP) & Project Guide - AngelScript LSP

AngelLSP is a high-performance C++20 Language Server Protocol (LSP) implementation for the AngelScript programming language (`.as` files), using Tree-Sitter for AST parsing and symbol table analysis.

---

## Core Architecture
- **Layer 4**: LSP Orchestrator & Server (`server/src/lsp/`, `server/src/main.cpp`)
- **Layer 3**: Feature Handlers (`server/src/features/`) - hover, definition, completion, semantic tokens, signature help.
- **Layer 2**: Analysis & Symbol Management (`server/src/analysis/`) - `SymbolTable`, `SymbolCollector`, `SymbolResolver`.
- **Layer 1**: Core, Document, Parser & Utilities (`server/src/document/`, `server/src/parser/`, `server/src/utils/`).

## Key Directories
- `server/`: C++20 LSP backend server (`CMakeLists.txt`, `src/`). Build directory: `server/build`.
- `client/`: VS Code extension TypeScript client.

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

---

## 1. The Unbreakable Rule of `#include`s (Layer Matrix)

To prevent circular dependencies and cascading build errors, review this matrix before writing any `#include`:

| Current File Layer | ✅ May `#include`: | ❌ FORBIDDEN to `#include`: |
| --- | --- | --- |
| **Layer 1: Core / Config**<br>(`config/`, `document/`, `parser/`, `utils/`) | Only standard C++ libraries (`<string>`, `<vector>`, etc.) or headers from its own layer. | Layer 2 (Analysis), Layer 3 (Features), Layer 4 (Server). |
| **Layer 2: Analysis**<br>(`analysis/`) | Layer 1 (Core/Config) and C++ libraries. | Layer 3 (Features), Layer 4 (Server). |
| **Layer 3: Features**<br>(`features/hover/`, `features/completion/`, etc.) | Layer 1 (Core) and Layer 2 (Analysis). | **Other Features** (e.g. Hover MUST NOT include Completion) and Layer 4 (Server). |
| **Layer 4: Server / Listener**<br>(`lsp/`, `main.cpp`) | Layer 1, Layer 2, and Layer 3. | None (topmost layer). |

> **Litmus test:** If you open `HoverHandler.h` and see `#include "../completion/CompletionHandler.h"`, **stop immediately**. That change breaks modularity.

---

## 2. Step-by-Step: How to Add or Refactor a Feature

When creating a new feature (e.g. `SignatureHelp`) or fixing an existing one, strictly follow these **5 steps**:

### Step 1: Define the pure contract (`SignatureHelpHandler.h`)

Create the function declaration in **Layer 3**. It must be a **pure** function accepting only const references (`const &`).

```cpp
#pragma once

#include "analysis/SymbolTable.h"
#include "document/Document.h"
#include <optional>

namespace lsp::features
{
    struct SignatureHelpRequest
    {
        const Document& document;
        const SymbolTable& symbolTable;
        Position position;
    };

    /**
     * @brief Computes signature help info for a function call at a given position.
     * @param request Immutable context needed to calculate signature help.
     * @return Optional SignatureHelp struct; nullopt if position is invalid.
     */
    std::optional<SignatureHelpResult> GetSignatureHelp(const SignatureHelpRequest& request);
}
```

### Step 2: Implement isolated logic (`SignatureHelpHandler.cpp`)

Write the code without storing global state or static variables.
* If the cursor is not over a valid function call, return `std::nullopt` or an empty result.
* **Never** throw exceptions (`throw`). Return an empty state if something fails.

### Step 3: Create the in-memory unit test (`tests/SignatureHelpTest.cpp`)

Test the function **without touching the disk** using the `TestUtils.h` helper:

```cpp
#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include "features/signature_help/SignatureHelpHandler.h"

TEST_CASE("ShouldProvideArgumentsForFunctionCall")
{
    // 1. Create fake in-memory document
    std::string code = "void test(int a, float b) {}\nvoid main() { test(|); }";
    auto doc = angel_lsp::test::CreateTestDocument("file:///test.as", code);
    angel_lsp::analysis::SymbolTable table;
    angel_lsp::test::PopulateTestSymbolTable(doc, table);

    // 2. Execute pure function...
}
```

### Step 4: Register the Kill-Switch and Capability

1. **Check `ServerConfig.h`:** Ensure the corresponding flag exists (`enableSignatureHelp`).
2. **Update `Server.cpp`:**
   * In `ComputeCapabilities()` / `Initialize`: Announce the capability to the client **only if** the flag is active.
   * In handlers: Wrap the call with the flag guard.

```cpp
messageHandler->add<lsp::requests::TextDocument_SignatureHelp>(
    [this](lsp::requests::TextDocument_SignatureHelp::Params &&req)
    {
        if (!m_config.features.enableSignatureHelp)
        {
            return lsp::requests::TextDocument_SignatureHelp::Result{};
        }
        // ...
    });
```

### Step 5: Compile and Validate

Run verification commands in your terminal:

```powershell
# 1. Build everything
cmake --build server/build --config Debug

# 2. Run test suite with CTest
cd server/build
ctest -C Debug --output-on-failure
```

---

## 3. Advanced Hybrid Architecture: Kimi K3 (Director) + Gemini `agy` (Executor)

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

---

## 4. Code Style and Documentation

1. **Allman Style**: Opening braces `{` always on a new line for classes, structs, functions, loops, and `if`/`switch`.
2. **Doxygen Comments in English**: Document every class, function, and struct using `/** ... */` format in English.
