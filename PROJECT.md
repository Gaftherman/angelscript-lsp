# Project: AngelScript Language Server (AngelLSP)

## Architecture
AngelLSP is a high-performance C++20 Language Server Protocol (LSP) implementation for AngelScript (`.as`), adhering strictly to a 4-layer unidirectional dependency architecture:
- **Layer 1: Core, Config, Document, Parser & Utilities** (`server/src/config/`, `server/src/document/`, `server/src/parser/`, `server/src/utils/`, `server/src/i18n/`)
- **Layer 2: Analysis & Symbol Management** (`server/src/analysis/` - `SymbolTable`, `ScopeTree`, `SymbolCollector`, `LocalScopeCollector`, `SemanticAnalyzer`, `TypeExtraction`)
- **Layer 3: Feature Handlers** (`server/src/features/` - `hover/`, `definition/`, `completion/`, `semantic_tokens/`, `signature_help/`)
- **Layer 4: LSP Orchestrator & Server** (`server/src/lsp/`, `server/src/main.cpp`)

### Include Matrix Rules
- Layer 1 includes only standard library or own layer headers.
- Layer 2 includes Layer 1 and standard library.
- Layer 3 includes Layer 1 and Layer 2. Features NEVER include other features.
- Layer 4 includes Layer 1, 2, and 3.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Primitive Types & Document Struct | Implement `IsPrimitiveType` in `Utils.cpp`, create `Document.h` | M1 | ORIGINAL_REQUEST §R3 |
| 2 | Hover Information | Hover tooltips with signature, types, and Doxygen doc extraction | M2 | ORIGINAL_REQUEST §R1 |
| 3 | Go to Definition & Type Definition | Definition & Type Definition lookup across local/global scopes | M2 | ORIGINAL_REQUEST §R1 |
| 4 | Auto-Completion | Lexical scope, member access (`.`), and qualifier (`::`) completion | M2 | ORIGINAL_REQUEST §R1 |
| 5 | Semantic Tokens | Full semantic tokens using `HIGHLIGHTS_QUERY` with delta encoding | M2 | ORIGINAL_REQUEST §R1 |
| 6 | Signature Help | Active parameter index and signature help in call expressions | M2 | ORIGINAL_REQUEST §R1 |
| 7 | Server Handler Registration | Register request handlers in `Server.cpp` (`InitHandles()`) | M3 | ORIGINAL_REQUEST §R2 |
| 8 | Dynamic Capability Announcement | Announce capabilities in `HandleRequestsInitialized` from `FeatureFlags` | M3 | ORIGINAL_REQUEST §R2 |
| 9 | CLI Argument Parsing | Parse CLI flags (`--enable-hover`, etc.) in `ServerConfig.cpp::FromArgs` | M3 | ORIGINAL_REQUEST §R2 |
| 10 | Feature Unit Test Suites | Doctest suites for Hover, Def, Completion, Tokens, SigHelp, Config | M4 | ORIGINAL_REQUEST §R3 |
| 11 | CTest 100% Verification | Build backend with 0 errors/warnings and verify all CTests pass | M4 | ORIGINAL_REQUEST Criteria |
| 12 | VS Code Client Verification | Ensure `npm run compile` in `client/` succeeds cleanly | M5 | ORIGINAL_REQUEST Criteria |
| 13 | Documentation Update | Update `README.md` to reflect architecture, CLI flags, test targets | M5 | ORIGINAL_REQUEST §R3 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | Core & Layer 1/2 Refinements | `Utils.cpp` (`IsPrimitiveType`), `Document.h`, CMake & Server sync fix | none | DONE |
| 2 | Layer 3 Feature Handlers | `features/` (Hover, Definition, Completion, SemanticTokens, SignatureHelp) | M1 | DONE |
| 3 | Layer 4 Server Wiring & CLI | `Server.cpp` (`InitHandles`, `HandleRequestsInitialized`), `ServerConfig.cpp` (`FromArgs`) | M2 | DONE |
| 4 | Comprehensive Test Suites & Verification | `tests/` feature doctest suites, CTest 100% pass verification | M3 | DONE |
| 5 | Client Build, Documentation & Audit | VS Code client compile, `README.md` update, Forensic Integrity Audit | M4 | DONE |

## Interface Contracts

### Layer 1: Document (`server/src/document/Document.h`)
```cpp
namespace angel_lsp::document {
    struct Document {
        std::string uri;
        std::string text;
        int version = 0;
        TSTree *tree = nullptr;
    };
}
```

### Layer 3: Feature Requests & Functions
- `HoverHandler`: `std::optional<lsp::Hover> GetHover(const HoverRequest &request);`
- `DefinitionHandler`: `std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest &request);`
- `DefinitionHandler`: `std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest &request);`
- `CompletionHandler`: `std::vector<lsp::CompletionItem> GetCompletion(const CompletionRequest &request);`
- `SemanticTokensHandler`: `lsp::SemanticTokens GetSemanticTokens(const SemanticTokensRequest &request);`
- `SignatureHelpHandler`: `std::optional<lsp::SignatureHelp> GetSignatureHelp(const SignatureHelpRequest &request);`

## Code Layout
- `server/src/document/Document.h`
- `server/src/utils/Utils.h`, `server/src/utils/Utils.cpp`
- `server/src/config/ServerConfig.h`, `server/src/config/ServerConfig.cpp`
- `server/src/features/hover/HoverHandler.h`, `HoverHandler.cpp`
- `server/src/features/definition/DefinitionHandler.h`, `DefinitionHandler.cpp`
- `server/src/features/completion/CompletionHandler.h`, `CompletionHandler.cpp`
- `server/src/features/semantic_tokens/SemanticTokensHandler.h`, `SemanticTokensHandler.cpp`
- `server/src/features/signature_help/SignatureHelpHandler.h`, `SignatureHelpHandler.cpp`
- `server/src/lsp/Server.h`, `server/src/lsp/Server.cpp`
- `server/tests/HoverTest.cpp`, `DefinitionTest.cpp`, `CompletionTest.cpp`, `SemanticTokensTest.cpp`, `SignatureHelpTest.cpp`, `ServerConfigTest.cpp`, `AdversarialFeaturesTest.cpp`
- `client/`
- `README.md`
