# Project: AngelLSP Phase 3 - Navigation/Semantic Fixes & Advanced LSP Capabilities

## Architecture
AngelLSP strictly enforces a 4-layer unidirectional architecture:
- **Layer 1: Core / Document / Parser / Utils / Config** (`server/src/document/`, `server/src/parser/`, `server/src/utils/`, `server/src/config/`)
  - AST parsing via Tree-sitter, Document text management, `IncludeResolver`, `ServerConfig`.
  - Allowed includes: Standard C++ and Layer 1 headers only.
- **Layer 2: Analysis & Symbols** (`server/src/analysis/`)
  - `SymbolTable`, `SymbolCollector`, `ScopeTree`, `LocalScopeCollector`, `SemanticHelpers`, `SemanticAnalyzer`.
  - Allowed includes: Layer 1, standard C++, and Layer 2 headers. FORBIDDEN: Layer 3 & Layer 4.
- **Layer 3: Feature Handlers** (`server/src/features/`)
  - Pure functions taking immutable request context (`const &`), returning `std::optional<Result>` or collections.
  - Existing Features: `hover/`, `definition/`, `completion/`, `semantic_tokens/`, `signature_help/`, `document_symbol/`, `workspace_symbol/`, `references/`, `rename/`.
  - Phase 3 Features: `document_highlight/`, `folding_range/`, `inlay_hint/`, `code_action/`, `formatting/`.
  - Allowed includes: Layer 1, Layer 2, standard C++, `<lsp/messages.h>`, `<lsp/types.h>`. FORBIDDEN: Other features, Layer 4.
- **Layer 4: Server Orchestrator** (`server/src/lsp/`, `server/src/main.cpp`)
  - Capability announcements, JSON-RPC request routing, configuration binding.
  - Allowed includes: Layers 1, 2, 3, and LSP framework.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Container & Inheritance-Aware Hover | Resolve symbols enclosed in classes (including base class hierarchy) and namespaces for hover signatures and docstrings | M1 | ORIGINAL_REQUEST §R1 |
| 2 | Container & Inheritance-Aware Definition | Navigate to definitions of class methods, inherited methods, and namespace functions | M1 | ORIGINAL_REQUEST §R1 |
| 3 | Container & Inheritance-Aware References & Rename | Find references and safely rename class methods, fields, and namespace functions across files | M1 | ORIGINAL_REQUEST §R1 |
| 4 | Primitive Types Semantic Tokens Fix | Tokenize primitive types (`float`, `int`, `bool`, etc.) as `Type_Keyword` without `Mod_DefaultLibrary` to restore `#FF7B72` color | M1 | ORIGINAL_REQUEST §R2 |
| 5 | Document Highlights | Highlight all read/write occurrences of symbol under cursor in current document (`textDocument/documentHighlight`) | M2 | ORIGINAL_REQUEST §R3 |
| 6 | Folding Ranges | Extract folding regions for classes, interfaces, namespaces, functions, control blocks, comments, and preprocessor `#if` | M2 | ORIGINAL_REQUEST §R4 |
| 7 | Inlay Hints | Provide parameter name hints for function/method calls and type deduction hints for `auto` variables (`textDocument/inlayHint`) | M3 | ORIGINAL_REQUEST §R5 |
| 8 | Code Actions | Provide quick-fix actions (remove unused variables, stub missing interface methods) via `textDocument/codeAction` | M3 | ORIGINAL_REQUEST §R6 |
| 9 | Document & Range Formatting | Configurable indentation (spaces/tabs), Allman brace alignment, and whitespace normalization (`textDocument/formatting`) | M4 | ORIGINAL_REQUEST §R7 |
| 10 | Server Configuration & Feature Flags | `ServerConfig.h/cpp` flags (`enableDocumentHighlight`, `enableFoldingRange`, `enableInlayHints`, `enableCodeAction`, `enableFormatting`) & CLI args | M5 | ORIGINAL_REQUEST §R8 |
| 11 | Layer 4 Capability & Handler Wiring | Register new handlers in `Server::InitHandles()`, announce capabilities in `Server::HandleRequestsInitialized()` | M5 | ORIGINAL_REQUEST §R8 |
| 12 | Comprehensive Doctest Test Suite | Unit & integration tests in `server/tests/` verifying all Phase 3 features with 100% CTest pass rate | M6 | ORIGINAL_REQUEST §R8 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | Fix Hover, Navigation & Primitive Semantic Tokens | Layer 2 `SemanticHelpers` container resolution, Layer 3 `HoverHandler`, `DefinitionHandler`, `ReferencesHandler`, `RenameHandler`, `SemanticTokensHandler` | none | DONE |
| 2 | Document Highlights & Folding Ranges | Layer 3 `DocumentHighlightHandler`, `FoldingRangeHandler`, `DocumentHighlightTest`, `FoldingRangeTest` | M1 | DONE |
| 3 | Inlay Hints & Code Actions | Layer 3 `InlayHintHandler`, `CodeActionHandler`, `InlayHintTest`, `CodeActionTest` | M1 | DONE |
| 4 | Document Formatting | Layer 3 `FormattingHandler`, `FormattingTest` | none | DONE |
| 5 | Server Wiring & Capability Registration | Layer 4 `Server.cpp` wiring, `ServerConfig.h/cpp` feature flags, `ServerConfigTest` | M1, M2, M3, M4 | DONE |
| 6 | Full Test Suite Pass & Adversarial Hardening | Comprehensive CTest execution, adversarial tests, edge cases, client compilation, Forensic Audit | M5 | IN_PROGRESS |


## Interface Contracts

### Layer 2: SemanticHelpers (`server/src/analysis/SemanticHelpers.h`)
```cpp
namespace angel_lsp::analysis
{
    std::string CleanBaseType(std::string_view typeName);
    std::vector<std::string> GetInheritedTypeHierarchy(
        const std::string& className,
        const SymbolTable& symbolTable);
    std::vector<std::string> GetAllRelatedClasses(
        const std::string& className,
        const SymbolTable& symbolTable);
    std::vector<std::string> GetEnclosingContainers(
        TSNode node,
        std::string_view sourceCode);
    std::vector<const Symbol*> FindSymbolsInScope(
        const std::string& name,
        TSNode node,
        std::string_view sourceCode,
        const SymbolTable& symbolTable);
}
```

### Layer 3: DocumentHighlightHandler (`server/src/features/document_highlight/DocumentHighlightHandler.h`)
```cpp
namespace angel_lsp::features
{
    struct DocumentHighlightRequest
    {
        const std::string& uri;
        const std::string& sourceCode;
        TSTree* tree = nullptr;
        lsp::Position position;
        const analysis::SymbolTable& symbolTable;
        const analysis::ScopeIndex& scopeIndex;
    };

    using DocumentHighlightResult = std::vector<lsp::DocumentHighlight>;
    std::optional<DocumentHighlightResult> GetDocumentHighlights(const DocumentHighlightRequest& request);
}
```

### Layer 3: FoldingRangeHandler (`server/src/features/folding_range/FoldingRangeHandler.h`)
```cpp
namespace angel_lsp::features
{
    struct FoldingRangeRequest
    {
        const std::string& uri;
        const std::string& sourceCode;
        TSTree* tree = nullptr;
    };

    using FoldingRangeResult = std::vector<lsp::FoldingRange>;
    std::optional<FoldingRangeResult> GetFoldingRanges(const FoldingRangeRequest& request);
}
```

### Layer 3: InlayHintHandler (`server/src/features/inlay_hint/InlayHintHandler.h`)
```cpp
namespace angel_lsp::features
{
    struct InlayHintRequest
    {
        const std::string& uri;
        const std::string& sourceCode;
        TSTree* tree = nullptr;
        lsp::Range range;
        const analysis::SymbolTable& symbolTable;
        const analysis::ScopeIndex& scopeIndex;
    };

    using InlayHintResult = std::vector<lsp::InlayHint>;
    std::optional<InlayHintResult> GetInlayHints(const InlayHintRequest& request);
}
```

### Layer 3: CodeActionHandler (`server/src/features/code_action/CodeActionHandler.h`)
```cpp
namespace angel_lsp::features
{
    struct CodeActionRequest
    {
        const std::string& uri;
        const std::string& sourceCode;
        TSTree* tree = nullptr;
        lsp::Range range;
        lsp::CodeActionContext context;
        const analysis::SymbolTable& symbolTable;
        const analysis::ScopeIndex& scopeIndex;
    };

    using CodeActionResult = std::vector<lsp::CodeAction>;
    std::optional<CodeActionResult> GetCodeActions(const CodeActionRequest& request);
}
```

### Layer 3: FormattingHandler (`server/src/features/formatting/FormattingHandler.h`)
```cpp
namespace angel_lsp::features
{
    struct FormattingRequest
    {
        const std::string& uri;
        const std::string& sourceCode;
        TSTree* tree = nullptr;
        lsp::FormattingOptions options;
    };

    struct RangeFormattingRequest
    {
        const std::string& uri;
        const std::string& sourceCode;
        TSTree* tree = nullptr;
        lsp::Range range;
        lsp::FormattingOptions options;
    };

    using FormattingResult = std::vector<lsp::TextEdit>;
    std::optional<FormattingResult> FormatDocument(const FormattingRequest& request);
    std::optional<FormattingResult> FormatRange(const RangeFormattingRequest& request);
}
```

## Code Layout
- `server/src/analysis/SemanticHelpers.h`, `server/src/analysis/SemanticHelpers.cpp`
- `server/src/features/hover/HoverHandler.cpp`
- `server/src/features/definition/DefinitionHandler.cpp`
- `server/src/features/references/ReferencesHandler.cpp`
- `server/src/features/rename/RenameHandler.cpp`
- `server/src/features/semantic_tokens/SemanticTokensHandler.cpp`
- `server/src/features/document_highlight/DocumentHighlightHandler.h`, `server/src/features/document_highlight/DocumentHighlightHandler.cpp`
- `server/src/features/folding_range/FoldingRangeHandler.h`, `server/src/features/folding_range/FoldingRangeHandler.cpp`
- `server/src/features/inlay_hint/InlayHintHandler.h`, `server/src/features/inlay_hint/InlayHintHandler.cpp`
- `server/src/features/code_action/CodeActionHandler.h`, `server/src/features/code_action/CodeActionHandler.cpp`
- `server/src/features/formatting/FormattingHandler.h`, `server/src/features/formatting/FormattingHandler.cpp`
- `server/src/config/ServerConfig.h`, `server/src/config/ServerConfig.cpp`
- `server/src/lsp/Server.h`, `server/src/lsp/Server.cpp`
- `server/tests/HoverTest.cpp`
- `server/tests/DefinitionTest.cpp`
- `server/tests/ReferencesTest.cpp`
- `server/tests/RenameTest.cpp`
- `server/tests/SemanticTokensTest.cpp`
- `server/tests/DocumentHighlightTest.cpp`
- `server/tests/FoldingRangeTest.cpp`
- `server/tests/InlayHintTest.cpp`
- `server/tests/CodeActionTest.cpp`
- `server/tests/FormattingTest.cpp`
- `server/tests/ServerConfigTest.cpp`

