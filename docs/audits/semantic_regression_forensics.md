# Semantic Regressions & Overload Ambiguities Forensic Report
**Repository:** `angelscript-lsp` | **Branch:** `develop`  
**Toolchain:** MSVC 19.50+ / GCC 13+ | Ninja Multi-Config | Super-Harness Active  
**Lead Auditor:** Principal C++20 Compiler Architect, Type System Specialist & Static Analysis Lead  
**Audit Date:** September 2026  

---

## 1. Executive Summary & Forensic Methodology

In accordance with the **Audit-First Directive**, an exhaustive diagnostic and forensic analysis was performed on branch `develop` to investigate **11 semantic defects, false positives, false negatives, and overload ambiguities** reported in real-world Sven Co-op scripts and `meta_api::json`.

Before introducing any patches to production code:
1. An empirical reproduction test suite was constructed in `server/tests/SemanticRegressionsBatch2Test.cpp` utilizing randomized symbol naming (`GenerateRandomSymbolName`) to ensure tests assert against invariant behaviors rather than hardcoded identifiers (complying with `AGENTS.md` Prohibition 10).
2. The reproduction suite was compiled and executed against the unmodified `develop` baseline.
3. Every test outcome was inspected across the AST parser, symbol collector, type synthesis, overload ranking engine, and diagnostic emission pipelines to determine the precise root causes.

### Baseline Summary Matrix

| Vector | Focus / Category | Status on Baseline | Emitted Diagnostics / Failure Mode | Root Cause Location |
| :--- | :--- | :--- | :--- | :--- |
| **V1** | Conditional Assignment Null Flow | **FAIL (FP)** | `as-warn-possible-null-dereference` on `monster` | `NullSafetyCondition.cpp:137`: `UnwrapNullExpression` does not unwrap `assignment_expression` to its target variable |
| **V2** | Disjunctive Short-Circuit Null Flow | **FAIL (FP)** | `as-warn-possible-null-dereference` on `hit` | `NullSafetyCondition.cpp:84`: `ProcessLogicalBinary` for `\|\|` drops positive assertions instead of computing $Pos(L) \cap Pos(R)$ |
| **V3** | Direct-Init Constructor (`const string&in`) | **PASS** | 0 diagnostics emitted | Handled cleanly by `CallChecker.cpp:CheckDeclaratorDirectInit` and `IsConvertible` |
| **V4** | Direct-Init Constructor (Default Arg) | **PASS** | 0 diagnostics emitted | `CallChecker.cpp:FilterMatchingArityCandidates` matches arity $[0, 1]$ correctly |
| **V5** | Overload Float vs Int Dominance | **FAIL (FP)** | `as-err-call-ambiguous` on `SetKeyvalue` | `ConversionRankingEngine.cpp:215`: `double -> float` ties with `double -> int` at `subRank = 10` |
| **V6** | Numeric Ambiguity on `Math.max` | **FAIL (FP)** | Ties `float` with `int64`/`uint64` | `ConversionRankingEngine.cpp:215`: Floating-to-floating narrowing ties with floating-to-integer truncation |
| **V7** | Enum to Arithmetic Type Promotion | **FAIL (FP)** | `as-err-no-implicit-conversion` (enum $\to$ float) | `TypeConversionChecker.cpp:679` and `ConversionRankingEngine.cpp:193` only allow conversion to integer types |
| **V8** | Ternary Array Evaluation | **FAIL (FP)** | `as-err-no-implicit-conversion` (float to float[]) | `SemanticHelpers.cpp:2619`: `ResolveTernaryExpr` calls `CleanBaseType`, stripping array brackets `[]` |
| **V9** | Multi-Hop Base Class Resolution | **FAIL (FP)** | `as-warn-undeclared-identifier: BaseClass` | `SemanticAnalyzer.cpp:920`: `IsAccessorPropertyOrKeyword` fails to recognize contextual keyword `BaseClass` |
| **V10** | L-Value Out-Param on Private Member | **FAIL (FP)** | `as-err-lvalue-required-for-out-param: m_defaults` | `CallChecker.cpp:1247`: `IsAssignableLValueSymbol` excludes `LocalDefinitionKind::Field` |
| **V11** | Qualified Type & Switch CFG Return | **PASS** | 0 diagnostics emitted | 4-level namespace type and switch CFG exhaustiveness analyze cleanly under standard formatting |

---

## 2. In-Depth Vector Forensics & Root Cause Mapping

### Vector 1: Conditional Assignment Null Flow
- **Target Construct:**
  ```as
  if (aiment.IsMonster() && (@monster = cast<CBaseMonster@>(aiment)) !is null) {
      monster.IsPlayer();
  }
  ```
- **Observed Baseline Behavior:** Emits `as-warn-possible-null-dereference` on `monster.IsPlayer()`.
- **AST Tracing:**
  - The condition is a `binary_expression` (`&&`).
  - The right operand is a `binary_expression` (`!is null`).
  - The left child of `!is null` is a `parenthesized_expression` enclosing `assignment_expression` (`@monster = cast<...>(...)`).
  - In `NullSafetyCondition.cpp:137`, `UnwrapNullExpression` only unwraps `parenthesized_expression` and `unary_expression`. It stops when encountering `assignment_expression`.
  - In `GetIdentifierName(node, sourceCode)`, the unwrapped node type is `assignment_expression`, which fails to match `identifier` or `scoped_identifier` and returns `""`.
  - As a result, no positive `NonNull` assertion is registered for `monster`, leaving its state as `Nullable` inside the `if` body.
- **Root Cause & Fix:**
  In `UnwrapNullExpression`, add handling for `parser::nodes::AssignmentExpression` by unwrapping to its `parser::fields::Left` child. When evaluating `(@monster = ...)` against `!is null`, the LHS unwraps to `@monster`, then to `monster`, correctly creating the `NonNull` assertion for `monster`.

---

### Vector 2: Disjunctive Short-Circuit Null Flow
- **Target Construct:**
  ```as
  if (hit !is null || (tr.pHit !is null && (@hit = g_EntityFuncs.Instance(tr.pHit)) !is null)) {
      hit.IsPlayer();
  }
  ```
- **Observed Baseline Behavior:** Emits `as-warn-possible-null-dereference` on `hit.IsPlayer()`.
- **AST Tracing:**
  - The condition is a `binary_expression` with operator `||`.
  - The LHS asserts `hit != null` ($posL = \{hit: NonNull\}$).
  - The RHS is a conjunction whose right conjunct asserts `@hit = ... != null` ($posR = \{tr.pHit: NonNull, hit: NonNull\}$).
  - In `NullSafetyCondition.cpp:84-88`:
    ```cpp
    else if (params.op == "||" || params.op == "or")
    {
        sink.negative.insert(sink.negative.end(), negL.begin(), negL.end());
        sink.negative.insert(sink.negative.end(), negR.begin(), negR.end());
    }
    ```
  - `sink.positive` is left completely empty!
- **Root Cause & Fix:**
  For a logical disjunction $A \lor B$, if variable $X$ is proven `NonNull` on the true branch of $A$, and $X$ is also proven `NonNull` on the true branch of $B$, then $X$ is guaranteed `NonNull` whenever $A \lor B$ evaluates to true.
  The positive assertions must be computed as $Pos(A \lor B) = Pos(A) \cap Pos(B)$.

---

### Vectors 3 & 4: Direct-Init Constructors
- **Target Constructs:**
  - `Logger g_Logger("JSON");` resolving to `Logger(const string&in Name)`
  - `Validator validator(strict);` resolving to `Validator(bool strict = false)`
- **Observed Baseline Behavior:** Both pass cleanly (0 constructor errors emitted).
- **Inspection & Invariant Verification:**
  - `CallChecker.cpp:CheckVariableDirectInitialization` walks all `variable_declaration` nodes at both local and file scope.
  - `CheckDeclaratorDirectInit` parses the arguments via `GetArgumentNodes(argListNode)` and inspects candidates via `LookupRawConstructors`.
  - In Vector 3: String literal `"JSON"` evaluates to `string`, which matches `const string&in` through `CanConvertBuiltins` / `IsConvertible`.
  - In Vector 4: `FilterMatchingArityCandidates` queries `ArityOf(fn)`, which produces `required = 0` and `maximum = 1`. Argument count 1 is within $[required, maximum]$, correctly selecting `Validator(bool strict = false)`.

---

### Vectors 5 & 6: Overload Float vs Int Dominance & Math.max Ambiguity
- **Target Constructs:**
  - `pCustom.SetKeyvalue(KVN_SHIELDSLAM, g_Engine.time + 0.1);` with overloads `SetKeyvalue(string, float)` and `SetKeyvalue(string, int)`.
  - `Math.max(0.1, (g_Engine.time > ... ? 1.0 : subsequent))` with overloads `max(float, float)`, `max(int64, int64)`, `max(uint64, uint64)`.
- **Observed Baseline Behavior:** Emits `as-err-call-ambiguous`.
- **AST Tracing & Ranking Forensics:**
  - In Vector 5, `g_Engine.time` is `float` and `0.1` is `double`. In `ResolveBinaryOpType`, `float + double` synthesizes to `double`.
  - In Vector 6, `0.1` is `double`, and ternary `1.0 : subsequent` synthesizes to `double`.
  - In `ConversionRankingEngine.cpp:215-219`:
    ```cpp
    if (IsPrimitiveNarrowing(ctx.cleanArg, ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 10, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Narrowing)};
    }
    ```
  - `IsPrimitiveNarrowing` returns `true` for `double -> float`, `double -> int`, `double -> int64`, and `double -> uint64`.
  - Crucially, all four conversions receive `ConversionRank::StandardConv` and `subRank = 10`.
  - When comparing candidate functions in `IsStrictlyBetter`, `conversions[i] <=> other.conversions[i]` compares `rank` (equal), `inheritanceDistance` (equal), `isConstAdjustment` (equal), and `subRank` (equal: $10 == 10$).
  - Neither candidate dominates the other. `CheckOverloadAmbiguity` detects equal-score non-dominated candidates and emits `as-err-call-ambiguous`.
- **Root Cause & Fix:**
  Floating-to-floating narrowing (`double -> float`) stays within the floating-point kind, preserving real-number semantics. Converting a floating-point number to an integer (`double -> int` / `double -> int64`) crosses the type kind, discarding fractional data.
  Assigning `subRank = 10` for floating-to-floating narrowing and `subRank = 30` (or 50 for unsigned) for kind-crossing truncation ensures `double -> float` strictly dominates `double -> int`.

---

### Vector 7: Enum to Arithmetic Type Promotion
- **Target Construct:**
  ```as
  enum SOUND_CHANNEL { CHAN_WEAPON = 1 }
  void PlaySound(float channel);
  PlaySound(CHAN_WEAPON);
  ```
- **Observed Baseline Behavior:** Emits `as-err-no-implicit-conversion: SOUND_CHANNEL, float`.
- **Analysis Forensics:**
  - `TypeConversionChecker.cpp:679`:
    ```cpp
    if (ResolvesToEnum(from, table) && parser::primitives::IsInteger(CanonicalizeType(to)))
    {
        return true;
    }
    ```
    Only checks `parser::primitives::IsInteger`, rejecting conversions to `float` and `double`.
  - `ConversionRankingEngine.cpp:193`:
    ```cpp
    if (IsIntegerType(ctx.cleanParam) && namesAnEnum(ctx.cleanArg))
    ```
    Only ranks conversions from enum to integer primitives. Calling `PlaySound(float)` with an enum returns `std::nullopt`, resulting in `ConversionRank::Incompatible`.
- **Root Cause & Fix:**
  Extend `TypeConversionChecker.cpp:679` to allow implicit conversion from enum to any numeric primitive (`parser::primitives::IsNumeric(to)`). In `ConversionRankingEngine.cpp`, rank enum-to-float/double with `ConversionRank::StandardConv, subRank = 30, penalty = WideningAcrossKind`.

---

### Vector 8: Ternary Expression Array Type Resolution
- **Target Construct:**
  ```as
  weapons::Accuracy(player, (type == AttackType::Primary) ? gp.primary_accuracy : gp.secondary_accuracy);
  ```
  Where `primary_accuracy` and `secondary_accuracy` are both `float[]`.
- **Observed Baseline Behavior:** Emits `as-err-no-implicit-conversion: float, float[]`.
- **AST Tracing & Forensics:**
  - In `SemanticHelpers.cpp:2619-2624`:
    ```cpp
    std::string c1 = CanonicalizeType(CleanBaseType(t1));
    std::string c2 = CanonicalizeType(CleanBaseType(t2));
    if (!c1.empty() && c1 == c2)
    {
        return (t1.ends_with("@") && t2.ends_with("@")) ? c1 + "@" : c1;
    }
    ```
  - `CleanBaseType("float[]")` is explicitly designed to unwrap element types and trailing decorations; it strips `[]` to `"float"`.
  - Since `c1 == "float"` and `c2 == "float"`, line 2623 returns `c1` (`"float"`), discarding the array brackets!
  - The argument to `weapons::Accuracy` (which expects `float[]`) is incorrectly inferred as scalar `float`.
- **Root Cause & Fix:**
  In `ResolveTernaryExpr`, compare `CleanExpressionType(t1) == CleanExpressionType(t2)` before stripping element types. When both branches possess the identical array or container type, return `CleanExpressionType(t1)` directly.

---

### Vector 9: Multi-Hop Base Class Resolution Across Predefined Stubs
- **Target Construct:**
  ```as
  class bts_rc_base_monster : ScriptBaseMonsterEntity {}
  class monster_parasite : bts_rc_base_monster {
      void CustomMethod(Task@ pTask) {
          @this.m_Schedules = null;
          BaseClass.RunTask(pTask);
      }
  }
  ```
  Where `ScriptBaseMonsterEntity` is defined in an `.as.predefined` stub.
- **Observed Baseline Behavior:** Emits `as-warn-undeclared-identifier: BaseClass`. Method call `RunTask` and field access `@this.m_Schedules` resolve successfully.
- **Analysis Forensics:**
  - `ResolveBaseClassIdentifier` in `SemanticHelpers.cpp:2105` successfully retrieves the base class `bts_rc_base_monster` and walks up to `ScriptBaseMonsterEntity` in the symbol table to resolve `RunTask`.
  - However, in `SemanticAnalyzer.cpp:950`, `CheckScopeReferences` iterates over all references in scope.
  - `BaseClass` is parsed by Tree-Sitter as an `identifier` (unlike `this`, which is parsed as a `this_expression`).
  - `CheckScopeReferences` calls `IsAccessorPropertyOrKeyword(ref, ctx)`.
  - `IsAccessorPropertyOrKeyword` only checks `IsReservedKeyword(ref.name)`, which queries the 53 strict AngelScript reserved keywords. Because `BaseClass` is a contextual keyword / convention in AngelScript game scripts, it is not in `k_reserved`.
  - `CheckScopeReferences` therefore emits `as-warn-undeclared-identifier: BaseClass`.
- **Root Cause & Fix:**
  In `SemanticAnalyzer.cpp:920` (`IsAccessorPropertyOrKeyword`), recognize `BaseClass` and `super` as valid contextual keyword identifiers.

---

### Vector 10: L-Value Output Parameter on Class Member Variable
- **Target Construct:**
  ```as
  void Deserialize(string config, json@ &out target);
  class ConfigManager {
      private json@ m_defaults;
      void Init() {
          Deserialize(this.__GetDefaultConfig__(), m_defaults);
      }
  }
  ```
- **Observed Baseline Behavior:** Emits `as-err-lvalue-required-for-out-param: m_defaults`.
- **Analysis Forensics:**
  - `CallChecker.cpp:1337` verifies out arguments with `CheckArgIsLValue`.
  - For identifier `m_defaults`, `CheckArgIsLValue` delegates to `IsAssignableLValueSymbol(aText, scope, table)`.
  - `IsAssignableLValueSymbol`:
    ```cpp
    bool IsAssignableLValueSymbol(std::string_view name, const Scope* scope, const SymbolTable& table)
    {
        if (scope)
        {
            const auto* def = ResolveInScope(scope, name);
            if (def && (def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter))
            {
                return true;
            }
        }
        ...
    }
    ```
  - In `LocalScopeCollector.cpp:82-83`, class member variables are classified as `LocalDefinitionKind::Field`.
  - `IsAssignableLValueSymbol` only permits `LocalDefinitionKind::Variable` and `LocalDefinitionKind::Parameter`, rejecting `LocalDefinitionKind::Field`!
- **Root Cause & Fix:**
  Update `IsAssignableLValueSymbol` in `CallChecker.cpp:1252` to accept `def->kind == LocalDefinitionKind::Field` (provided it is not `const`).

---

### Vector 11: Multi-Segment Scoped Types & Switch CFG Return Exhaustiveness
- **Target Construct:**
  ```as
  bool Check(const meta_api::json::v2::Null&in value) {
      switch(this.Type) {
          case JT_Null: return true;
          default: return false;
      }
  }
  ```
- **Observed Baseline Behavior:** Passes cleanly (0 diagnostics emitted).
- **Forensic Verification:**
  - `TypeExtraction.cpp:255` extracts scope prefix `meta_api::json::v2::` and base type `Null`, forming the fully qualified type.
  - In `ControlFlowChecker.cpp:251`, `SwitchDefinitelyReturns` verifies that all clauses return and a `default:` clause is present, accurately confirming CFG exhaustiveness.

---

## 3. Targeted Remediation Plan

All remediations will adhere strictly to `AGENTS.md` standards:
1. Max 4 parameters per function.
2. Lizard Cyclomatic Complexity $\le 15$ CCN, lines $\le 70$.
3. File line limit $\le 300$ net LOC.
4. No cross-feature or cross-layer protocol leaks.

### Execution Plan:
1. **Fix 1 (Vectors 1 & 2):** In `server/src/analysis/NullSafetyCondition.cpp`:
   - Unwrap `parser::nodes::AssignmentExpression` in `UnwrapNullExpression`.
   - Compute positive assertion intersection for `||` in `ProcessLogicalBinary`.
2. **Fix 2 (Vectors 5 & 6):** In `server/src/analysis/overload/ConversionRankingEngine.cpp`:
   - Prioritize floating-to-floating narrowing (`subRank = 10`) over floating-to-integer kind crossing (`subRank = 30` / `50`).
3. **Fix 3 (Vector 7):** In `server/src/analysis/TypeConversionChecker.cpp` & `ConversionRankingEngine.cpp`:
   - Permit implicit promotion from enum to any numeric primitive (`parser::primitives::IsNumeric(to)`).
   - Rank enum $\to$ float/double with `ConversionRank::StandardConv, subRank = 30`.
4. **Fix 4 (Vector 8):** In `server/src/analysis/SemanticHelpers.cpp`:
   - In `ResolveTernaryExpr`, preserve matching container and array types via `CleanExpressionType`.
5. **Fix 5 (Vector 9):** In `server/src/analysis/SemanticAnalyzer.cpp`:
   - In `IsAccessorPropertyOrKeyword`, recognize `BaseClass` as a valid contextual receiver keyword (while preserving strict base-constructor constraints on `super`).
6. **Fix 6 (Vector 10):** In `server/src/analysis/CallChecker.cpp`:
   - In `IsAssignableLValueSymbol`, permit `LocalDefinitionKind::Field`.

---

## 4. Post-Remediation Verification & Quality Scorecard

### 4.1. Vector Resolution Status

| Vector | Focus / Category | Baseline Status | Post-Remediation Status | Verification Notes |
| :--- | :--- | :--- | :--- | :--- |
| **V1** | Conditional Assignment Null Flow | FAIL (FP) | **PASS** | `monster` correctly tracked as `NonNull` inside `if` body |
| **V2** | Disjunctive Short-Circuit Null Flow | FAIL (FP) | **PASS** | `hit` correctly tracked as `NonNull` across `||` branches |
| **V3** | Direct-Init Constructor (`const string&in`) | PASS | **PASS** | Resolves to `Logger(const string&in)` cleanly |
| **V4** | Direct-Init Constructor (Default Arg) | PASS | **PASS** | Matches `Validator(bool strict = false)` cleanly |
| **V5** | Overload Float vs Int Dominance | FAIL (FP) | **PASS** | `SetKeyvalue(string, float)` dominates `SetKeyvalue(string, int)` |
| **V6** | Numeric Ambiguity on `Math.max` | FAIL (FP) | **PASS** | `Math.max(float, float)` unambiguously wins over integer overloads |
| **V7** | Enum to Arithmetic Type Promotion | FAIL (FP) | **PASS** | `SOUND_CHANNEL` promotes to `float` with zero diagnostics |
| **V8** | Ternary Array Evaluation | FAIL (FP) | **PASS** | Ternary preserves `float[]` array bracket type |
| **V9** | Multi-Hop Base Class Resolution | FAIL (FP) | **PASS** | `BaseClass.RunTask` resolves through script to stub base |
| **V10** | L-Value Out-Param on Private Member | FAIL (FP) | **PASS** | `m_defaults` accepted as assignable L-value field |
| **V11** | Qualified Type & Switch CFG Return | PASS | **PASS** | Multi-level namespace and exhaustive switch validated |

### 4.2. Quality Gate Verification

```
[RUNNING] Layer Architecture Invariants...
[PASSED] Layer Architecture Invariants

[RUNNING] Clean Function Signatures...
[PASSED] Clean Function Signatures

[RUNNING] Diagnostic Codes Registry...
[PASSED] Diagnostic Codes Registry

[RUNNING] Code Duplication (jscpd)...
[PASSED] Code Duplication (jscpd: 2.13% <= 3.0%)

[RUNNING] Cyclomatic Complexity (Lizard)...
[PASSED] Cyclomatic Complexity (Lizard: max CCN <= 15, max LOC <= 70)

[RUNNING] AST Antipattern Gate (ast-grep)...
[PASSED] AST Antipattern Gate (ast-grep)

[SUCCESS] All static quality gates passed successfully.
```

### 4.3. Test Suite Regression Invariant

- **Targeted Suite (`SemanticRegressionBatch2`):** 11 / 11 test cases passed (21 / 21 assertions, 100%).
- **Full Test Suite:** 1,993 / 1,993 test cases passed (78,966 / 78,966 assertions, 0 failures, 0 regressions).

