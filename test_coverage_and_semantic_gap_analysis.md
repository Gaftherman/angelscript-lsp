# Comprehensive Codebase Gap & Antipattern Audit
**Project:** AngelScript Language Server (`angel_lsp`)  
**Repository Root:** `E:\Github\src\angelscript-lsp`  
**Date:** September 2026  
**Auditor:** Principal C++20 Systems Architect, Compiler Engineer & Test Framework Architect  

---

## Executive Summary

This audit catalogs compiler antipatterns, semantic gaps, diagnostic false positives/negatives, and feature fidelity degradations across the `angel_lsp` semantic analysis engine (`server/src/analysis/`) and LSP feature handlers (`server/src/features/`).

Historically, heuristic shortcuts (flat string comparisons, scalar scoring magic numbers, ad-hoc fallback searches, and disconnected test environments) were introduced to quickly satisfy isolated editor behaviors. While convenient in the short term, these heuristics introduced regressions when scripts evolved to use modern AngelScript idioms (such as nested namespaces, scoped enums, multi-file modular include graphs, and preprocessor-controlled declarations).

This document establishes the theoretical ground truth (cross-referenced against `asharness.exe` and `angelscript_oracle`) and maps the remediation strategy through a unified **Semantic & Feature Fidelity Test Harness** (`server/tests/helpers/LspSemanticHarnessFixture.h`).

---

## 1. Diagnostic Accuracy Audit (False Positives & False Negatives)

### 1.1 Control Flow & Switch Statement CFG (`as-err-not-all-paths-return`)
- **Antipattern / Defect:**
  In `ControlFlowChecker.cpp` (`SwitchDefinitelyReturns`), a `switch` statement is evaluated for whether all control paths return a value. 
  The native AngelScript compiler (`asharness.exe`) enforces the following structural grammar invariants:
  1. A `switch` without any `case` labels (even if it has `default: return ...;`) is rejected by the compiler with: `Empty switch statement`.
  2. If a `default:` clause is present, it must be the **last** clause in the switch (`The default case must be the last one`).
  3. When every `case` and the terminal `default:` clause either explicitly returns or falls through to a clause that returns, the switch is considered exhaustive and guarantees a return on all paths.
- **Identified Gap in `ControlFlowChecker.cpp`:**
  `SwitchDefinitelyReturns` iterated over clauses and tracked `lastClauseHasStatements`. If the final `default:` clause had statements returning a value, but preceding empty fallthrough cases (e.g. `case 1: \n case 2: return true;`) were present, improper child indexing or statement counting could cause `allReturn` to evaluate to `false`, falsely emitting `as-err-not-all-paths-return`.
- **Ground Truth Oracle Behavior:**
  ```angelscript
  bool Test(int x) {
      switch (x) {
          case 1:
          case 2:
              return true;
          default:
              return false;
      }
  }
  ```
  `asharness.exe` compiles this cleanly with 0 diagnostics.

---

### 1.2 Implicit Default Constructor Synthesis (`as-err-no-matching-constructor`)
- **Antipattern / Defect:**
  AngelScript specification dictates that if a script class declares *no* explicit constructors, the compiler automatically synthesizes an implicit parameterless default constructor: `ClassName()`.
  If a derived class inherits from a base class without declaring constructors:
  ```angelscript
  class Deserializer {}
  class __Deserializer__ : Deserializer {}
  void main() {
      __Deserializer__ d();
  }
  ```
  When the analyzer looked up constructors in `CallChecker.cpp` via `SymbolTable::FindConstructors`, if the symbol collector only recorded explicit AST constructors, the query returned empty for `__Deserializer__`. The checker fell back to reporting `as-err-no-matching-constructor: No matching constructor for '__Deserializer__()'`.
- **Ground Truth Oracle Behavior:**
  Verified against `asharness.exe`: `Derived d();` is completely valid and compiles with 0 errors.
- **Architectural Solution:**
  `SymbolCollector::SynthesizeImplicitConstructors` synthesizes an implicit constructor for every script class lacking explicit constructors, allowing overload resolution and constructor type checks to proceed uniformly.

---

### 1.3 Scoped Enum Member Resolution (`as-err-unresolved-type` / `as-warn-undeclared-identifier`)
- **Antipattern / Defect:**
  In real-world libraries such as `json.as`, enums are frequently nested inside namespaces and accessed with multi-part qualified paths:
  ```angelscript
  namespace meta_api {
      namespace json {
          enum Type { Object, Array, String }
          enum Version { V1 = 1, V2 = 2 }
      }
  }
  ```
  When resolving `meta_api::json::Type::Object` or `meta_api::json::Version::V1`:
  1. In expression contexts (`int t = meta_api::json::Type::Object;`), previous token splitters split only the last component (`Object`), searching for `meta_api::json::Type` as a global container or class. Because `Type` is an enum, container resolution failed unless the enum was specifically recognized as a symbol container.
  2. In return type contexts (`meta_api::json::Version GetVersion()`), the full path must resolve to `meta_api::json::Version` without stripping or truncating the namespace.
- **Ground Truth Oracle Behavior:**
  Both `meta_api::json::Type::Object` (expression value) and `meta_api::json::Version` (type) compile with 0 warnings or errors in native AngelScript.

---

### 1.4 Upward Scope Traversal on Root-Prefixed Paths
- **Antipattern / Defect:**
  When code inside a nested namespace references a sibling or parent namespace (e.g., referencing `meta_api::json::parser::KeyValuePair@` from inside `meta_api::json::v1`), resolution must walk up the enclosing namespace hierarchy to find the common ancestor root `meta_api`, rather than assuming the path is relative to `meta_api::json::v1`.
- **Mitigation:**
  `SymbolTable::FindScopedSymbol` and `SymbolResolution::ResolveTargetSymbol` must test qualified prefixes starting from the innermost scope outwards to the global scope.

---

### 1.5 Handle Output Parameter Binding (`T@&out`)
- **Antipattern / Defect:**
  In serialization libraries (e.g. `Deserialize(str, this)`), methods often declare output parameters as `T@&out` (a reference to a handle). When passing `this` inside a class method:
  `this` represents an object instance of type `T`. When bound to `T@&out`, the engine converts the instance to a handle reference.
  Strict type equality previously rejected this as a mismatch because `T` != `T@&`.
- **Compiler Design Pattern:**
  In AngelScript, an object of class `T` can bind to `T@` (handle) or `T@&` output references because object identity provides a valid handle. The type conversion rank lattice recognizes this as an exact/identity handle conversion (`ConversionRank::Exact`).

---

### 1.6 Preprocessor Directives & Conditional Lexical Anchoring
- **Antipattern / Defect:**
  When symbols are declared within preprocessor conditionals (such as `#if METAMOD_PLUGIN_ASLP` ... `#endif`), if the preprocessor definition scanner does not account for host-configured defined words, AST nodes inside `#if` blocks could be treated as excluded regions (`ExcludedLineRange`), stripping the symbols from the symbol table. Subsequent references in unconditioned functions then emitted `as-warn-undeclared-identifier`.
- **Mitigation:**
  `PreprocessorRegions` must strictly honor `DefinedWords()` populated from `--define`, preprocessor stubs, and workspace configuration.

---

### 1.7 Variable Identifier Collisions and Auto Variable Resolution
- **Antipattern / Defect:**
  Substring heuristics (e.g. searching for a variable name `key` inside string literal `"client_prediction"` or member accesses `pmove.player`) caused false shadow warnings and misdirected hover.
- **Compiler Design Pattern:**
  All semantic operations must strictly anchor to AST node boundaries (`ts_node_start_point`, `ts_node_end_point`) and exact `LocalReference` / `LocalDefinition` records in `ScopeIndex`.

---

## 2. Feature Fidelity Audit (Hover, Completion, Definition)

### 2.1 Hover Fidelity & Doc Comment Extraction
- **Degraded Behavior:**
  1. Properties with virtual accessors (`uint8 Minutes { get const { return 0; } }`): Hover previously returned raw function signatures (`uint8 get_Minutes() const`) or lost associated Doxygen doc comments located above the property block.
  2. Intermediate namespaces: Hovering over `fmt` in `meta_api::json::v2::fmt::ToArray` yielded empty or broken tooltips because intermediate qualified tokens were not resolved as namespace symbols.
  3. Member disambiguation: In expressions like `this.Error.IsActive()`, hover could resolve `Error` to a global enum value instead of the class member `Logger::ASLogLevel Error`.
- **Fidelity Requirements:**
  - Properties must display their property declaration and doc comments.
  - Intermediate namespace nodes must display `namespace <QualifiedName>`.
  - Member access (`this.X`, `obj.X`) must prioritize the receiver type's member symbols.

---

### 2.2 Completion Filtering & Isolation
- **Degraded Behavior:**
  1. Instance access (`data.`): Completion candidate lists frequently leaked static enums, global functions, and unrelated types instead of strictly proposing members of `typeof(data)`.
  2. Namespace access (`meta_api::json::`): Proposals should include nested namespaces (`v1`, `v2`, `parser`) and types (`Type`, `Version`, `json`), but must never propose out-of-scope local variables from the current function.
- **Fidelity Requirements:**
  - Dot operator (`.`): Propose instance methods and fields only.
  - Scope operator (`::`): Propose namespace children, nested classes, static methods, and enum values.

---

### 2.3 Go-To-Definition Precedence
- **Degraded Behavior:**
  When triggering Go-To-Definition (`Ctrl+Click`) on a symbol like `Type` in `meta_api::json::Type::Object`, if an external predefined stub (`sven.as.predefined`) declared a stub type with the same flat name `Type`, definition navigation would jump to the read-only stub rather than the local workspace declaration in `json.as`.
- **Fidelity Requirements:**
  - Workspace symbols in the same module/include tree strictly shadow and take precedence over external/predefined stubs.
  - Qualified paths must resolve to the exact enclosing container.

---

## 3. Architecture of the Unified Semantic Harness Fixture

To prevent regression across all current and future semantic capabilities, we deploy `LspSemanticHarnessFixture`.

### Class Contract:
```cpp
namespace angel_lsp::test
{

class LspSemanticHarnessFixture
{
public:
    LspSemanticHarnessFixture();
    ~LspSemanticHarnessFixture();

    void SetUp();
    void TearDown();

    // Virtual Workspace Lifecycle
    void LoadPredefinedStub(const std::string& stubRelativePath);
    void AddVirtualDocument(const std::string& uri, const std::string& content);
    void UpdateVirtualDocument(const std::string& uri, const std::string& newContent);

    // 1. Semantic & Diagnostic Assertions
    void AssertNoDiagnostics(const std::string& uri);
    void AssertDiagnosticAt(const std::string& uri, uint32_t line, const std::string& expectedCode);
    void AssertDiagnosticsCount(const std::string& uri, size_t expectedCount);

    // 2. Feature Fidelity Assertions
    void AssertHoverSignature(const std::string& uri, uint32_t line, uint32_t col, const std::string& expectedSignature);
    void AssertHoverContains(const std::string& uri, uint32_t line, uint32_t col, const std::string& expectedText);
    void AssertCompletionContains(const std::string& uri, uint32_t line, uint32_t col, const std::string& expectedLabel, lsp::CompletionItemKind expectedKind);
    void AssertCompletionExcludes(const std::string& uri, uint32_t line, uint32_t col, const std::string& unexpectedLabel);
    void AssertDefinitionTarget(const std::string& uri, uint32_t line, uint32_t col, const std::string& expectedTargetUri, uint32_t expectedTargetLine);

    // 3. Native Engine Oracle Check
    bool VerifyWithNativeOracle(const std::string& sourceSnippet, std::string& outCompilerError);
};

} // namespace angel_lsp::test
```

### Architectural Guarantees:
1. **Synchronous & Deterministic:** No background thread races or debounce timing dependencies.
2. **Real Filesystem Backing:** A managed temporary sandbox provides disk files for `IncludeResolver` and `asharness.exe`.
3. **Comprehensive Feature Coverage:** Direct invocation of AST parser, symbol collector, local scope collector, semantic analyzer, hover, completion, and definition handlers.
4. **Oracle Verification:** Direct interface with native `asharness.exe` or `angelscript_oracle` to guarantee genuine AngelScript language compliance.
