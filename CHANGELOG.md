# Change Log

All notable changes to the "angelscript-lsp" extension will be documented in this file.

Check [Keep a Changelog](http://keepachangelog.com/) for recommendations on how to structure this file.

## [0.7.7-exp.16] - 2026-09-11

### Fixed

- Compiler Warnings and Cross-Platform Diagnostics:
  - Added missing `NonInstantiableKind::Mixin` case in `VariableRules.cpp` to resolve GCC/Clang `-Wswitch` warnings.
  - Corrected nested aggregate initialization for `lsp::Range` subobjects in `ClassRules.cpp` to eliminate `-Wmissing-braces` warnings.
  - Eliminated dead file-scope `k_...FieldLength` constants across analysis and feature handlers (`AccessChecker`, `CallChecker`, `CallGraph`, `ConstChecker`, `ControlFlowChecker`, `InitializerListChecker`, `TypeConversionChecker`, `CallHierarchyHandler`) to resolve `-Wunused-const-variable` warnings.
  - Removed unused lambda captures in `Server.cpp` and `ServerHarnessTest.cpp`.
  - Stored base class collection in local variable before `fmt::join` formatting in `Server.cpp` to prevent GCC `-Wdangling-reference` warnings.

## [0.7.7-exp.15] - 2026-09-11

### Fixed

- Invalidate Semantic Tokens Delta Cache on Syntax Error Transitions:
  - Added syntax error state tracking in `Server::SemanticTokensSnapshot`.
  - When transitioning into or out of syntax error recovery states (`ts_node_has_error`), `TextDocument_SemanticTokens_Full_Delta` returns full `SemanticTokens` directly, resetting the client's token buffer and preventing desynchronization.
- Enforce Strictly Aligned 5-Tuple Edits in `ComputeSemanticTokensDelta`:
  - Enforced that prefix, suffix, start offset, delete count, and replacement data slices in `ComputeSemanticTokensDelta` are strictly aligned to multiples of 5 integers (whole tokens).
  - Eliminated sub-token splicing bugs that caused modulo-5 misalignment and permanently shifted token types into length and offset fields.
- Strict Operator Semantic Token Classification and Punctuation Exclusion:
  - Removed `punctuation.bracket` mapping to operator tokens, preventing `{`, `}`, `(`, `)`, `[`, `]` from ever receiving `Type_Operator`.
  - Added strict operator validation (`IsGenuineOperator` and `IsPunctuationOrBracket`), ensuring brackets and delimiters (`{`, `}`, `(`, `)`, `[`, `]`, `;`, `,`) can never be tagged as operators even during AST fallback traversal.
  - Added a safety post-filter on filtered semantic tokens to guarantee no non-operator node is ever emitted as an operator.

## [0.7.7-exp.14] - 2026-09-11

### Fixed

- Ternary Expression Same-Type Deduction for Complex Classes:
  - Resolved false-positive type deduction failure where ternary expressions with identical complex classes (e.g. Vector) evaluated to unknown ("").
  - Added identity type matching in ResolveExpressionType for ternary expressions after type canonicalization.
  - Corrected constructor call return type resolution in ResolveExpressionType for class and interface types.
- Primitive Integer Alias Canonicalization:
  - Unified integer alias canonicalization (int32 -> int, uint32 -> uint, short -> int16, ushort -> uint16) across SemanticHelpers, TypeConversionChecker, and OverloadResolver.
  - Updated IsSameType and IsConvertible in TypeConversionChecker to treat aliased integer types as equivalent, eliminating spurious conversion diagnostics in variable initializations, assignments, and calls.
- Implicit Enum-to-Integer Conversions:
  - Allowed implicit conversion from enum types to integer primitives in TypeConversionChecker and SemanticHelpers.
  - Prevented registered or local enum declarations from being misclassified as unresolved engine-level types.
  - Permitted ternary expressions combining enum and integer values to promote to integer types without emitting false-positive conversion diagnostics.

## [0.7.7-exp.13] - 2026-09-11

### Added

- Engine Parity via asharness: Bare Type Names in Expression Contexts:
  - Added diagnostic `as-err-expression-is-data-type` (`ExpressionIsDataType`) in English and Spanish.
  - Reject bare data type names (classes, interfaces, enums, typedefs, funcdefs, primitive types) passed as function call arguments or used as stand-alone expressions.
  - Implemented `IsBareDataType` helper distinguishing value expressions from bare type symbols.
- Go-to-Definition (F12) and Implementation (Ctrl+F12) inside Mixin Declarations:
  - F12 Go-to-Definition on declaration node returns its own definition range to highlight the symbol.
  - Indexed `RuleIndex::hostClassesByMixin` to track host classes including each mixin.
  - Ctrl+F12 on mixin method declarations queries all host classes, reporting explicit overrides when declared and the host class declaration when not overridden.
- Client Configurable Debug Log Level:
  - Added `angelscript.server.logLevel` setting (`error`, `warn`, `info`, `debug`, `trace`, default `debug`) in client configuration with English and Spanish translations.
  - Wired `--log-level=${level}` server startup argument in `extension.ts`.
- Ternary Expression Type Mismatch and Call Parameter Validation:
  - Ensured incompatible branches (string vs non-string, conflicting enum types) in ternary expressions emit `as-err-no-implicit-conversion`.
  - Emitted parameter type mismatch diagnostics in `CallChecker` when call arguments contain malformed ternary expressions.
- Predefined Stub Re-indexing Latency Optimizations:
  - Added microsecond timing probes across `ParserPredefined`.
  - Added fast-path bypass in `didOpen` for unchanged `.as.predefined` stub content to avoid redundant include-graph rebuilds and workspace-wide re-analysis.

## [0.7.7-exp.12] - 2026-09-11

### Added

- Null-Safe Server Logging and Predefined Stub Analysis Guards:
  - Added null-safe forwarding log helpers (`LogInfo`, `LogWarning`, `LogError`, `LogDebug`, `LogTrace`) in `Server.h` and `Server.cpp`, preventing crashes when handlers or background tasks log without an active logger instance.
  - Early-returned in `LogRule` and `LogParam` in `DiagnosticContext.cpp` when `logger` is null or debug logging is disabled.
  - Guarded `ReanalyseOpenDocuments` and `ScheduleOpenDocumentsForReanalysis` to skip `.as.predefined` stub files, breaking circular re-analysis loops on stub modification.
- Bare Type Temporaries in Expression Type Deduction:
  - Extended `ResolveExpressionType` in `SemanticHelpers.cpp` to resolve bare type identifiers (classes, interfaces, enums, typedefs, and core primitives) as their type name (e.g. `Vector` resolves to `"Vector"`).
  - Eliminates false positive argument type mismatch errors when using default constructor syntax without explicit parenthesized instantiation.
- Full InlayHint Parameter Label Preservation:
  - Verified and ensured parameter hint labels are never artificially truncated, maintaining full descriptive identifiers (e.g. `shouldTrace:`).
- Mixin Go-to-Implementation Navigation Fallback:
  - Enhanced `ImplementationHandler.cpp` to inspect enclosing class `includedMixins` across the full inheritance hierarchy on unqualified member calls.
  - Falls back to the physical declaration site in `sym.fileUri` when no derived class overrides exist.

## [0.7.7-exp.9] - 2026-09-11

### Added

- Method Overload Arity and Signature Isolation in References and CodeLens:
  - Added call tracking and argument counting (`isCall`, `argumentCount`) to `LocalReference` during lexical scope collection (`LocalScopeCollector.cpp`).
  - Added function signature metadata (`isFunction`, `minArgs`, `maxArgs`) to `TargetDescriptor` in `SymbolResolution.h`.
  - Implemented safe extraction of `FunctionSignature` in `SymbolResolution.cpp` without dangling pointers or lifetime hazards during overload resolution.
  - Applied strict argument arity filtering on call references across class members, namespace symbols, and global functions.
  - Sifted out non-matching overload declarations by gathering all declaration line ranges across `SymbolResolution.cpp` and `scanScopes`, preventing distinct overloads from being misclassified as call sites.
  - Updated `CodeLensHandler.cpp` to filter reference counts by function arity constraints.
  - Completely eliminated reference overcounting across overloads (e.g. `Deploy()` 0-arg overload cleanly separated from 6-arg overloads).
- Go-to-Implementation Definition Fallback:
  - Added fallback to symbol definition when 0 implementations or derived overrides exist for types (`Class`, `Interface`), member methods/properties, and free functions in `ImplementationHandler.cpp`.
  - Conforms to standard LSP navigation behaviors implemented by clangd and TypeScript language service.
- InlayHint Argument Cutoff Resolution on Complex Calls:
  - Added receiver object text fallback in `ResolveExpressionType` (`SemanticHelpers.cpp`) for dot-accessed namespace and static class invocations (`Math.RandomLong`).
  - Updated `ScoreArgumentMatch` in `OverloadResolver.cpp` to return `OverloadMatchPenalty::UnknownTypes` when argument types cannot be statically deduced, preventing viable overloads from being dropped.
  - Added fallback candidate selection by highest parameter count in `InlayHintHandler.cpp` (`ResolveCalleeParameters`), ensuring complete parameter name hints on complex nested calls (`EmitSoundDyn`, `Math.RandomLong`).
- Native Assets and Package Branding:
  - Generated crisp 128x128 RGBA extension icon (`client/assets/icon.png`).
  - Created scalable SVG file icons for AngelScript files and predefined headers (`client/assets/as-file-icon.svg`, `client/assets/as-predefined-icon.svg`).
  - Registered extension icon and file icons in `client/package.json`.

## [0.7.7-exp.8] - 2026-09-11

### Added

- Strict Scope Isolation in Reference Resolution & CodeLens:
  - Added `GetDerivedClasses` and `GetCompatibleMemberClasses` in `SemanticHelpers` to enforce object-oriented access boundaries and inheritance constraints across classes and interfaces.
  - Sibling class isolation: Sibling classes sharing a common base class (such as weapon entities deriving from `ScriptBasePlayerWeaponEntity`) no longer leak references or inflate CodeLens counts.
  - Strict access modifier enforcement:
    - `private` members are strictly isolated to the declaring class AST scope; sibling classes, subclasses, and superclasses are excluded.
    - `protected` members are restricted to the declaring class and its direct/indirect derived classes; sibling classes sharing a base class are excluded.
    - `public` members resolve to the highest ancestor declaring the member; sibling classes that independently declare same-named members without a common ancestor declaration are treated as distinct symbols.
  - `TargetDescriptor` now carries `analysis::AccessModifier access` resolved dynamically from the symbol definition or inheritance hierarchy.
  - Member access reference resolution now uses backward scope scanning in addition to active AST parsing to accurately determine receiver types across files without full re-parsing.
  - Comprehensive unit regression tests added to `ReferencesTest.cpp` and `CodeLensTest.cpp`.
- Anti-Hardcoding Audit on Virtual Document URI Extraction:
  - Modernized `ExtractVirtualHostClass` and `ExtractVirtualMixinName` in `SymbolTable.cpp` using robust, scheme-agnostic token parsing.
  - Eliminated hardcoded string offset constants, safely supporting arbitrary scheme representations (`angelscript-virtual:`, `angelscript-virtual://`, `angelscript-virtual:///`) and arbitrary namespace nesting (such as `Game::Weapons::Rifle` and URL-encoded variants).
  - Confirmed zero linear `ForEachSymbol` iterations introduced in URI extraction and reference collection hot paths.

## [0.7.7-exp.7] - 2026-09-11

### Added

- Native Inline Peek Inspection:
  - CodeLens above mixin inclusion lines now triggers VS Code's native inline peek widget (`editor.action.peekLocations`) pointing directly to the mixin declaration site without altering editor split layout.
  - Added dedicated command `angelscript.peekMixinInline` with fallback to virtual document split view in headless environments.
- Host-Scope Fallback Resolution in Virtual Documents:
  - Hover tooltips and Go-to-Definition in virtual documents (`angelscript-virtual://<host>/<mixin>.as`) now seamlessly resolve host members, properties, methods, and base class inherited members (such as `self`, `m_pPlayer`, and weapon state accessors).
  - Variable and property signature formatting enhanced to show full type annotations for class properties (`(property) CBasePlayerWeapon@ self`).
  - Added full test coverage for virtual document host-scoped hover and definition resolution in `VirtualMixinDocumentTest.cpp`.

### Optimized

- Cold Document-Open Latency:
  - Debounced synchronous semantic analysis and module closure indexing during `textDocument/didOpen` when document symbols are already indexed or an analysis pass is pending.
  - Quick local scope and AST parse pass provides instantaneous navigation and syntax diagnostics while full semantic checking runs asynchronously.

## [0.7.7-exp.6] - 2026-09-10

### Added

- Semantic Mixin Instantiation Checking:
  - Deep AST-level validation of mixin member statements against host class scope and inheritance hierarchy.
  - Emits diagnostic `as-err-mixin-instantiation-member-not-found` when mixin accesses unresolved host properties or methods.
  - Attach LSP `relatedInformation` pointing to the host class declaration and mixin inclusion statement.
  - Complete bilingual localization (EN / ES) for mixin instantiation diagnostics.
- Dedicated Mixin UI Commands & CodeLens:
  - Added `angelscript.viewMixinExpansion` command and editor context menu option to inspect synthesized mixin documents in split view.
  - Added `angelscript.openPhysicalSource` command to return to physical mixin definitions from virtual documents.
  - Added CodeLens on mixin inclusion lines (`[View Mixin Expansion: <MixinName>]`) to open the virtual expanded document.
  - Added header CodeLens at top of virtual mixin documents (`[Jump to physical source in <filename>]`) to navigate back to physical code.
  - Complete bilingual localization across client manifest (`package.nls.json`, `package.nls.es.json`) and runtime bundles (`bundle.l10n.json`, `bundle.l10n.es.json`).
- Virtual Document Scope Resolution:
  - Resolves `this` and `self` receiver accesses inside virtual mixin documents to the synthetic host class.
  - Added hover tooltips and definition routing for `this` and `self` targeting host class declarations.

### Fixed

- Physical Go-to-Definition (F12) Restored:
  - Standard F12 definition requests now reliably return the physical source file (`file://...`) where mixin templates are defined rather than virtual documents.

## [0.7.7-exp.5] - 2026-09-10

### Added

- Virtual Mixin Document Inspection:
  - Full mixin code expansion in virtual documents.
  - Client virtual document selector and integration.
  - Overload fallback heuristic scoring when callee parameter types are partially known.
  - Scoped CodeLens references on synthesized mixin declarations.

## [0.7.7-exp.4] - 2026-09-10

### Added

- Client-side configuration support for `angelscript.enableVirtualMixinDocuments` in `package.json` under resource scope.
- Client launch argument forwarding: passing `--enable-virtual-mixin-documents=true` to the language server process during initialization when enabled.
- Setting `angelscript.inlayHints.suppressWhenArgumentMatchesName` exposed in the client configuration schema and forwarded as `--inlay-hints-suppress-when-argument-matches-name`.
- Full internationalization and localization across English (`package.nls.json`) and Spanish (`package.nls.es.json`) schemas for all new configuration properties.

### Changed

- Updated client setting wiring validation (`check-settings-wired.mjs`) to enforce 100% translation key alignment and argument verification across all configuration options.

## [0.7.7-exp.3] - 2026-09-10

### Added

- Language Server Protocol Call Hierarchy provider (`textDocument/prepareCallHierarchy`, `callHierarchy/incomingCalls`, `callHierarchy/outgoingCalls`):
  - Resolves target functions and method symbols under the cursor, including synthesized mixin members.
  - Incoming calls query workspace-wide call sites targeting functions or synthesized host overloads.
  - Outgoing calls traverse AST function bodies; synthesized host callers resolve back to originating mixin declarations to locate outbound invocations accurately.
- Virtual Mixin Document infrastructure scaffolding behind `--enable-virtual-mixin-documents=[true|false]`:
  - Added URI helper and schema generator for `angelscript-virtual://<host_class>/<mixin>.as`.
  - Added `virtualFileUri` metadata field on `Symbol` instances to support future AST-level virtual document expansion.

### Fixed

- CodeLens mixin deduplication: grouped symbols sharing exact declaration ranges to emit strictly one CodeLens button per mixin method declaration.
- Aggregated reference counts across the mixin declaration and all synthesized host class overloads, eliminating stacked duplicate CodeLens lines and matching the true deduplicated reference list.
- Cascading peer open-document re-analysis debouncing: introduced a 250ms coalescing window with steady-clock tracking (`m_peerAnalysisTimestamps`) in `ReanalyseOpenDocuments` and `DidSave` cascades, preventing re-analysis storms across peer documents such as `base.as`.

### Changed

- Transitioned default setting for parameter inlay hints: `inlayHintsSuppressWhenArgumentMatchesName` is now `false` by default, displaying parameter hints for identifiers matching argument names unless explicitly suppressed.

## [0.7.7-exp.2] - 2026-09-10

### Added

- Overload-aware Go-to-Definition (`FilterOverloadsForCall`):
  - Callee argument type scoring deduces expression types via `ResolveExpressionType` and ranks candidates with `ResolveBestOverload`.
  - Arity and parameter count fallback heuristic scoring when exact types cannot be resolved.
- Parameter inlay hints for `BaseClass` method invocations with hierarchy traversal that bypasses mixins to target true base classes.
- Parameter inlay hints for global and utility objects (`Math.MakeVectors`).

### Fixed

- Mixin origin navigation: jumping to synthesized mixin methods now navigates directly to the mixin's origin declaration file URI and precise line range rather than host class locations.
- Document symbol erasure and invalidation: host class synthesized member cleanup now erases synthesized symbols unconditionally during document updates, preventing stale symbol accumulation.

## [0.7.7-exp.1] - 2026-09-10

### Performance

- Diagnostic checker bypass for `.predefined` engine stubs, reducing workspace stub indexing overhead by over 95%.
- Save Storm elimination: implemented an interface hash guard utilizing an FNV-1a 64-bit ABI fingerprint to prevent cascading re-analysis when public signatures remain unmodified.
- Eradicated linear `symbolTable.ForEachSymbol` iterations in hot performance paths (hover evaluation, document highlights, and enum member resolution) in favor of keyed bucket lookups.
- Added high-resolution microsecond telemetry instrumentation across workspace indexing and document analysis stages.

### Fixed

- Native compiler parity verified via `asharness.exe` for isolated expression statements (`null;`, integer literals, floating-point literals, and boolean literals).
- Predefined stub parsing robustness and handle-to-reference binding resolution.

## [0.7.6-exp.1] - 2026-09-10

### Tested

- Parity audit and reinforcement for `InitializerListChecker` against reference AngelScript compiler harness (`asharness`).
- Verified list pattern edge cases for multidimensional and nested container constructors.

## [0.7.5-exp.1] - 2026-09-10

### Refactored

- Dynamic `SymbolTable` aggregate inspection replacing ad-hoc type names (`grid`, `complex`, `vector`).
- Eliminated explicit dictionary string checks, unifying list pattern validation with engine behaviour.
- Verified initializer list parity against test corpus.

## [0.7.4-exp.1] - 2026-09-09

### Added

- Multi-argument overload cost scoring vector with Pareto dominance resolution.
- Lexical scope barrier for closures (`ScopeKind::Closure`) isolating local captures.
- Unified boolean truthiness validation in control flow conditions via `IsTruthyCondition`.
- First-principles mixin class semantics and deferred interface method checking.
- Automated git tag-based naming for VSIX packages and server binaries.

## [0.7.3] - 2026-09-09

### Fixed

- Method overload resolution across multi-level class inheritance hierarchies.
- Hover tooltips and inlay hints for inherited properties and methods.
- Duplicate diagnostic reporting on file save events.

## [0.7.2] - 2026-09-09

### Fixed

- Receiver type resolution for chained member expressions (`a.b.c`).
- Population of default values for enum constants and parameters in hover tooltips.
- Prioritization of scoped identifiers (`Namespace::Symbol`) over local shadowing.

## [0.7.1] - 2026-09-09

### Refactored

- Removed hardcoded container names and ad-hoc fallback heuristics across semantic analysis passes.
- Standardized container inspection using dynamic symbol table definitions.

## [0.7.0] - 2026-09-09

### Added

- Global virtual property accessor support (`get_`/`set_`).
- Chained member auto-completion suggestions.

### Fixed

- Doxygen doc-comment parsing with newline preservation and trailing comment extraction.
- Prioritized release server binaries during client launcher discovery.
- Eliminated performance lag and false-positive diagnostics when parsing large `.predefined` stubs.
- Restricted wildcard `?` type exemptions strictly to predefined stubs.

## [0.6.2] - 2026-09-09

### Fixed

- Hot-reloading of workspace module configuration changes.
- Automatic purging of stale unconfigured closure files from server cache.

## [0.6.1] - 2026-09-08

### Added

- Folder-based module configuration support in client settings.
- Server-side closure cache retention across edits.

## [0.6.0] - 2026-09-08

### Fixed

- Analysis pipeline concurrency: unified document mutations through a serialized queue funnel to prevent background thread races.
- List pattern validation handling when opening `.predefined` stubs as active editor documents.

## [0.5.0] - 2026-09-08

### Added

- Enhanced auto-completion shapes: function signatures with interactive argument placeholders.
- Keyword completion for `class`, `interface`, and control statements.
- Semantic diagnostic: standalone anonymous function expressions reported as compiler errors.
- Preprocessor line highlighting with precise directive tokenization.

## [0.4.0] - 2026-08-30

### Added

- Clangd-style Doxygen Markdown rendering in hover tooltips (`@brief`, `@param`, `@return`, `@see`).
- Support for folder-based script modules (`folder` setting) and workspace-wide `#include` script resolution.
- Variable expansion support (`${workspaceFolder}`, `${env:VAR}`) in client path settings.
- Operator overload resolution covering all 52 AngelScript operator signatures and `cast<T>()`.
- Add-on types declared in standard engine profiles.

### Fixed

- L-value assignment checking for conditional ternary expressions.
- Enum member qualification in namespace scopes.

## [0.3.0] - 2026-08-27

### Added

- Language Server Protocol pull diagnostics support (`textDocument/diagnostic`).
- Type checking: condition expressions must evaluate to boolean types.
- Reporting of duplicate case labels and unrecognized engine profile configurations.
- Predefined stub name indicator in status bar.
- Snippet completions for anonymous functions.

### Fixed

- Unlinked `#if` directive dimming matching standard C++ preprocessor visuals.
- Semantic token colors aligned across eleven AST node categories.
- Namespace collision detection when reopened across multiple module files.

## [0.2.0] - 2026-08-24

### Fixed

- Real-time diagnostic refresh during active typing.
- Server restart queuing to eliminate race conditions when toggling extension settings.
- Diagnostic codes aligned with compiler error names: `as-err-null-non-handle`, `as-warn-undeclared-identifier`.
- Property completion and hover respecting `asEP_PROPERTY_ACCESSOR_MODE`.

### Added

- Predefined stub selection picker with notification action button.
- Bilingual localization (English and Spanish) across all settings and UI messages.
- Status bar item with quick access to server logs, restart command, and stub picker.

### Changed

- Extension client bundled into a single distribution file using esbuild.

## [0.1.0] - 2026-08-20

- Initial release of AngelScript Language Server.
- Native C++20 backend utilizing Tree-Sitter for AST parsing.
- Core LSP features: diagnostics, hover, definition, completion, semantic tokens, signature help, and document symbols.
