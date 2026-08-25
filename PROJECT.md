# Project: AngelScript Language Server (AngelLSP) — Phase 4

## Architecture
- **Layer 1: Core / Config / Document / Parser / Utils** (`server/src/config/`, `server/src/document/`, `server/src/parser/`, `server/src/utils/`)
  - Standalone data structures, configuration options, Tree-Sitter AST parser wrappers, and include path utilities.
  - `#include` rules: only standard library and Layer 1 headers.
- **Layer 2: Analysis & Symbol Management** (`server/src/analysis/`)
  - `SymbolTable`, `ScopeTree`, `SemanticHelpers`, `ConstChecker`, `CallChecker`, `ControlFlowChecker`, `SemanticAnalyzer`, `DefiniteAssignmentChecker`, `OverloadResolver`, `EngineProfiles`.
  - `#include` rules: Layer 1 headers and standard library.
- **Layer 3: Feature Handlers** (`server/src/features/`)
  - Pure function handlers: `HoverHandler`, `DefinitionHandler`, `ReferencesHandler`, `RenameHandler`, `DocumentSymbolHandler`, `WorkspaceSymbolHandler`, `DocumentHighlightHandler`, `FoldingRangeHandler`, `InlayHintHandler`, `CodeActionHandler`, `FormattingHandler`.
  - `#include` rules: Layer 1 and Layer 2 headers. No cross-feature includes!
- **Layer 4: LSP Orchestrator & Server** (`server/src/lsp/`, `server/src/main.cpp`)
  - Protocol message routing, capability negotiation, document lifecycle events, config synchronization.
  - `#include` rules: Layers 1, 2, and 3.

## Feature Inventory
| # | Feature | Description | Milestone | Source | Status |
|---|---------|-------------|-----------|--------|--------|
| 1 | Deep Expression Type Deduction | Deduces types across binary operators, ternary expressions, member/method call chains (`obj.a().b().c`), and nested templates (`array<dictionary<K,V>>`) | M1 | Survey R1 | DONE |
| 2 | Argument Type Overload Resolution | Multi-tier scoring ranking candidate overloads by argument type compatibility (exact, const/ref, subtype, widening, narrowing) | M1 | Survey R1 | DONE |
| 3 | Definite Assignment Analysis | Dataflow tracking detecting uninitialized local variable reads across control branches | M1 | Survey R1 | DONE |
| 4 | Extract Variable Refactoring | Generates `refactor.extract` code action extracting selected expression into a local variable | M2 | Survey R2 | DONE |
| 5 | Extract Method Refactoring | Generates `refactor.extract` code action extracting selected statements into a function/method with parameter/return deduction | M2 | Survey R2 | DONE |
| 6 | Getter/Setter Generation Quick Fix | Generates Allman-formatted `get_Prop() const` and `set_Prop(Type val)` methods for class fields | M2 | Survey R2 | DONE |
| 7 | Missing `const` Method Qualifier Quick Fix | Quick fix for `as-err-const-method-required` diagnostic and intention for non-mutating methods | M2 | Survey R2 | DONE |
| 8 | Sort and Clean `#include` Directives | `source.organizeImports` action grouping `<...>` and `"..."`, sorting alphabetically, and removing unused includes | M2 | Survey R2 | DONE |
| 9 | Predefined Engine Stub Profiles | Built-in embedded stubs (`standard`, `svencoop`, `urho3d`, `openxray`, `ootp`, `auto`) loaded under synthetic URIs | M3 | Survey R3 | IN_PROGRESS |
| 10 | Multi-Platform CI/CD Matrix & Packaging | GitHub Actions workflow for Windows, Linux, and macOS (x64/arm64) matrix build, test, and VSIX extension packaging | M3 | Survey R3 | IN_PROGRESS |
| 11 | Rich VS Code AngelScript Snippets | Comprehensive snippet collection (`client/snippets/angelscript.json`) registered in `client/package.json` | M3 | Survey R3 | IN_PROGRESS |
| 12 | Comprehensive E2E Test Suite | 4-tier requirement-driven test suite verifying all Phase 4 features with 100% CTest pass | M4 | Survey Acceptance | PLANNED |

## Milestones
| # | Name | Scope | Dependencies | Status | Key Outputs |
|---|------|-------|-------------|--------|-------------|
| 1 | M1: Semantic Analysis & Type Inference | Deep expression type deduction, overload resolution, definite assignment checker | None | DONE | `SemanticHelpers`, `OverloadResolver`, `DefiniteAssignmentChecker`, 838 tests |
| 2 | M2: Code Actions & Refactorings | Extract Variable/Method, Getter/Setter generation, const quick fixes, sort/clean includes | M1 (types) | DONE | `CodeActionHandler` (Extract Var/Method, Getters/Setters, Const, Includes), 883 tests |
| 3 | M3: CI/CD, Snippets & Engine Profiles | Built-in engine profiles, GitHub Actions multi-platform matrix, VS Code snippets | None | IN_PROGRESS | Ready for implementation dispatch |
| 4 | M4: E2E Integration & Verification | E2E test verification, cross-feature validation, adversarial hardening | M1, M2, M3 | PLANNED | - |

## Interface Contracts
### `SemanticHelpers` ↔ Feature Handlers & Analyzers
- `std::string ResolveExpressionType(TSNode exprNode, const Document &doc, const SymbolTable &table, const ScopeIndex *scopeIndex = nullptr)`
- Evaluates binary operators (`+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `&&`, `||`, `&`, `|`, `^`, etc.), member chains, index expressions, ternary expressions, and template types.

### `OverloadResolver` ↔ `CallChecker`, `HoverHandler`, `InlayHintHandler`
- `struct OverloadCandidate { const Symbol* symbol; int score; };`
- `std::optional<const Symbol*> ResolveBestOverload(const std::vector<const Symbol*> &candidates, const std::vector<std::string> &argTypes, const SymbolTable &table)`

### `DefiniteAssignmentChecker` ↔ `SemanticAnalyzer`
- `std::vector<Diagnostic> CheckDefiniteAssignment(TSNode rootNode, const Document &doc, const SymbolTable &table, const ScopeIndex &scopeIndex)`

### `CodeActionHandler` ↔ `Server`
- `std::vector<lsp::CodeAction> GetCodeActions(const CodeActionRequest &request)`
- Handles `RefactorExtract` (Extract Variable, Extract Method), `QuickFix` (Getters/Setters, Const qualifier), and `SourceOrganizeImports` (Sort & clean includes).

### `EngineProfiles` ↔ `ServerConfig` & `Server`
- `std::string_view GetProfileStubContent(EngineProfile profile)`
- `std::vector<std::pair<std::string, std::string>> GetBuiltinProfileDocuments(const ServerConfig &config)`

## Code Layout
- `server/src/analysis/SemanticHelpers.h/cpp` (Extended expression type deduction)
- `server/src/analysis/OverloadResolver.h/cpp` (Overload scoring and resolution)
- `server/src/analysis/DefiniteAssignmentChecker.h/cpp` (Definite assignment analysis)
- `server/src/analysis/EngineProfiles.h/cpp` (Built-in engine stub definitions)
- `server/src/features/code_action/CodeActionHandler.h/cpp` (Extract, getters/setters, const, include organize)
- `server/src/config/ServerConfig.h/cpp` (Engine profile and feature flag configuration)
- `server/src/lsp/Server.cpp` (Engine profile ingestion and capability routing)
- `server/tests/` (In-memory doctest unit tests)
- `client/snippets/angelscript.json` (AngelScript language snippets)
- `client/package.json` (Extension contributions: snippets, engineProfile settings)
- `.github/workflows/ci.yml` (Multi-platform GitHub Actions workflow)
