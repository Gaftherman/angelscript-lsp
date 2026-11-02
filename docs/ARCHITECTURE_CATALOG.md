# AngelLSP Architecture Catalog

## 1. Architectural Overview & Strict Layer Matrix

AngelLSP is engineered under a strict unidirectional 4-layer dependency model enforced at compile-time and through automated CI quality gates:

| Layer | Path | Allowed to Include | Strictly Forbidden to Include |
| :--- | :--- | :--- | :--- |
| **Layer 1: Core / Config** | `core/`, `config/`, `document/`, `parser/`, `utils/`, `i18n/` | Own layer, standard C++ libraries | Layers 2, 3, and 4 |
| **Layer 2: Analysis** | `analysis/` | Layer 1, standard C++ libraries | Layers 3 and 4 |
| **Layer 3: Features** | `features/<feature_name>/` | Layers 1 and 2 | Sibling features, Layer 4 |
| **Layer 4: Server / LSP** | `lsp/`, `main.cpp` | Layers 1, 2, and 3 | None (topmost layer) |

---

## 2. Layer 1: Core, Config, Parser & Utilities

- **`config/`**: Server configuration flags (`ServerConfig.h`), engine rule configurations (`EngineRuleConfig.h`).
- **`document/`**: Thread-safe document handles (`Document.h`, `DocumentId.h`), UTF-8 line/character offset mapping (`PositionCodec.h`).
- **`parser/`**: Tree-Sitter AngelScript C++ parser bindings (`AngelScriptParser.h`), precompiled grammar symbol constants (`GrammarSymbols.h`), static AST utilities and parent navigation (`ASTUtils.h`).
- **`utils/`**: High-performance utilities:
  - `ScopedNameUtils.h`: Zero-allocation string view scope qualifiers.
  - `WorkspaceIncludeGraph.h`: Thread-safe include dependency tracking.
  - `MemoryTelemetry.h`: OS process memory counters and latency telemetry.
  - `Timer.h`: Sub-millisecond high-resolution timing.
  - `StopFlag.h`: Early cancellation for analysis passes.
- **`i18n/`**: Central multi-language localization dictionary (`i18n.h`, `i18n.cpp`) supporting English and Spanish with zero hardcoded user-facing strings.

---

## 3. Layer 2: Analysis (Domain-Driven Design Sub-Layers)

### Sub-Layer 2.1 (Index): AST Indexing & Lexical Scopes
- **`analysis/index/NodeIndex.h` / `.cpp`**: Single-pass preorder Tree-Sitter AST index bucketing nodes by symbol in $O(1)$. Manages `IndexedContainerSpan` (compacted to 20 bytes) for container intervals, comments, and syntax error recovery.
- **`analysis/index/ScopeTree.h` / `.cpp`**: Lexical scope hierarchy. Defines `Scope` (compacted to 128 bytes) and `LocalReference` (compacted to 80 bytes).
- **`analysis/index/LocalScopeCollector.h` / `.cpp`**: Extracts lexical scopes, local variable definitions, and references in Pass 1.
- **`analysis/index/SymbolTable.h` / `.cpp`**: Thread-safe symbol store supporting copy-on-write pointer snapshots (`FindSymbolsPtr`, `FindMemberSymbolPtr`).
- **`analysis/index/Symbol.h`**: Canonical definition of `struct Symbol`, `SymbolType`, `AccessModifier`, and `SymbolModifiers` (4-byte bitfields).
- **`analysis/index/SymbolCollector.h` / `.cpp`**: Pre-pass extracting global and container-scoped symbols from document ASTs.
- **`analysis/index/CallGraph.h` / `.cpp`**: Function and method invocation call graph.
- **`analysis/index/DeclarationRecovery.h` / `.cpp`**: Resilient parsing and symbol recovery for incomplete declarations during editing.

### Sub-Layer 2.2 (Types): Type System & Representations
- **`analysis/types/ExpressionTypeCache.h`**: Thread-safe memoization cache for resolved AST expression types and poison sentinels.
- **`analysis/types/TypeSanitization.h`**: Zero-allocation type modifier helpers (`StripLeadingConst`, `StripTrailingDecorationsView`, `CleanBaseType`).
- **`analysis/types/TypeExtraction.h`**: Extraction of return types, parameter types, and variable types from Tree-Sitter nodes.
- **`analysis/types/EngineProfiles.h`**: Configurable predefined engine profiles (Vanilla AngelScript, Sven-Co-op, etc.).
- **`analysis/types/VirtualMixinContext.h`**: Virtual document mapping and line tracking for AngelScript mixin expansions.

### Sub-Layer 2.3 (Diagnostics): Diagnostics & Suppressions
- **`analysis/diagnostics/Diagnostics.h`**: Diagnostic representation (`DiagnosticSeverity`, `SourceRange`, `DiagnosticRelatedInformation`).
- **`analysis/diagnostics/DiagnosticCodes.h`**: Static constants for all AngelScript diagnostic codes.
- **`analysis/diagnostics/DiagnosticContext.h` / `.cpp`**: Context for accumulating and formatting diagnostics.
- **`analysis/diagnostics/DiagnosticSuppression.h` / `.cpp`**: Inline comment-based diagnostic suppressions (`// disable W156`).

### Sub-Layer 2.4 (Documentation): Comments & Markdown Rendering
- **`analysis/documentation/DocComment.h` / `.cpp`**: Javadoc and Doxygen doc comment parsing.
- **`analysis/documentation/DoxygenMarkdown.h` / `.cpp`**: Markdown rendering for hover tooltips.
- **`analysis/documentation/SignatureFormatter.h` / `.cpp`**: Canonical text representation for hover signatures.

### Sub-Layer 2.5 (Resolution): Semantic Resolution Engines
- **`analysis/resolution/ClassHierarchyResolver.h` / `.cpp`**: Inheritance graph resolution, base class lookup, and mixin expansion.
- **`analysis/resolution/ExpressionTypeResolver.h` / `.cpp`**: Expression typing, operator return types, and member receiver typing.
- **`analysis/resolution/TemplateTypeResolver.h` / `.cpp`**: Dynamic template argument binding and generic substitution.
- **`analysis/resolution/PropertyAndDelegateResolver.h` / `.cpp`**: Getter/setter property resolution and delegate target identification.
- **`analysis/resolution/CallableAndLambdaResolver.h` / `.cpp`**: Argument extraction, funcdef resolution, and lambda signature inference.
- **`analysis/resolution/ScopeSymbolLookup.h` / `.cpp`**: Visible symbol lookup within enclosing lexical scopes and `using namespace` directives.
- **`analysis/resolution/TargetResolution.h` / `.cpp`**: Semantic jump-to-target and cross-file occurrence resolution.
- **`analysis/resolution/OverloadResolver.h` / `.cpp`**: Best-match overload selection and ranking.

### Sub-Layer 2.6 (Rules & Checkers): Semantic Language Rules
- **`analysis/rules/`**: Declaration and semantic rules (`ClassRules`, `FunctionRules`, `OperatorRules`, `TypeRules`, `VariableRules`, `EngineRuleChecker`, `UnusedSymbolChecker`, `DirectiveRules`).
- **`analysis/checkers/`**: Semantic AST checkers (`AccessChecker`, `BinaryOperatorChecker`, `CallChecker`, `ComparisonOperatorChecker`, `ConstChecker`, `ControlFlowChecker`, `DefiniteAssignmentChecker`, `IsolationChecker`, `LValueChecker`, `NamespaceChecker`, `RepeatedConversionChecker`, `TypeConversionChecker`, `UndefinedIdentifierChecker`, and `null_safety/`).

### Sub-Layer 2.7: Semantic Mediator & Coordinator
- **`analysis/mediator/SemanticMediator.h` / `.cpp`**: GoF Mediator coordinating `SymbolTable`, `ScopeTree`, `NodeIndex`, and resolution engines in $O(1)$.
- **`analysis/SemanticAnalyzer.h` / `.cpp`**: Top-level semantic analysis orchestrator constructing mediator passes and executing all checkers.

---

## 4. Layer 3: Feature Handlers

All LSP language features reside in `server/src/features/` and operate purely on Layer 1 and Layer 2 APIs with zero sibling cross-includes:
- `features/hover/`: Hover tooltips and type introspection.
- `features/completion/`: Context-aware completion suggestions.
- `features/definition/`: Jump-to-definition for symbols and includes.
- `features/references/`: Find all references across open files and workspace index.
- `features/rename/`: Safe multi-file semantic rename with conflict detection.
- `features/document_symbol/`: Hierarchical document outline.
- `features/workspace_symbol/`: Fast workspace-wide fuzzy symbol search.
- `features/folding_range/`: Streaming zero-allocation syntax and preprocessor folding.
- `features/inlay_hint/`: Parameter name and inferred variable type inlay hints.
- `features/code_lens/`: Reference count and implementation lenses.
- `features/semantic_tokens/`: Delta semantic token classification.
- `features/type_hierarchy/`: Supertype and subtype tree navigation.

---

## 5. Layer 4: Server Core & Protocol Wiring

- **`lsp/state/`**: In-memory server state (`DocumentStore`, `WorkspaceStateStore`, `ModuleIndex`, `PredefinedStubManager`).
- **`lsp/pipeline/`**: Asynchronous scheduling and background worker pipeline (`AnalysisScheduler`, `DiagnosticsPipeline`).
- **`lsp/services/`**: Domain services (`ServerAnalysis`, `ServerDiagnostics`, `ServerEngineRules`, `ServerModules`).
- **`lsp/handlers/`**: LSP protocol request and notification handlers.
- **`lsp/Server.h` / `Server.cpp`**: Server lifecycle coordinator and JSON-RPC message router.

---

## 6. Structural & Memory Ceilings

To ensure sub-10ms response times and predictable memory consumption under large game codebases (e.g. Sven-Co-op):
- `sizeof(IndexedContainerSpan) <= 20 bytes` (achieved via 15-bit TSSymbol and bitfield packing).
- `sizeof(LocalReference) <= 80 bytes` (achieved via byte offsets replacing 32-byte string qualifier).
- `sizeof(Scope) <= 128 bytes` (achieved via compact flat representation and bitfield flags).
- Every feature request targets $\le 10\text{ ms}$ server processing latency.
