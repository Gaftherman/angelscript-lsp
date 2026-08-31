# Parity backlog

An issue list from a second AngelScript language server was distilled into ~60 items covering the
parser, the type system, scope, properties, lambdas, `as.predefined`, completion and the protocol.
This file records what happened when each claim was put to a real compiler, and what is left to
build.

The suite here passes at 1240 with zero unexplained false positives across six corpora, so nothing
on that list would ever have surfaced from the tests alone. That is the blind spot this file exists
to cover.

## The rule this file enforces

**A claim about AngelScript is worth nothing until the oracle has answered it.** Nine of the
document's claims are wrong, and four of them describe behaviour this analyzer already has correct —
implementing them as written would have replaced working rules with false positives.

Ask first:

```bash
cmake -B build -S server -DANGELLSP_BUILD_ORACLE=ON
cmake --build build --config Release --target angelscript_oracle
build/Release/angelscript_oracle yourcase.as
```

Exit 0 means the compiler accepts the file, exit 1 means it rejects it. Then write the case into
`server/tests/parity/` so the answer is kept rather than remembered.

## Corpus prefixes

Every claim below has a script in `server/tests/parity/`:

| Prefix | Meaning |
|---|---|
| `doc_p*` | The compiler **accepts** it, and so do we. These are the regression guards. |
| `doc_r*` | The compiler **rejects** it. Several of these refute the document outright. |
| `doc_g*` | The compiler accepts it and we did **not**. Listed in `KnownGaps()` in `ParityAuditTest.cpp` while the gap is open; the entry comes out when it closes, and the file stays as an accept/accept guard. Both current `doc_g` files are closed. |

---

## Refuted — do not implement

| Document item | What it claims | What the compiler said | What that means here |
|---|---|---|---|
| `SCOPE-08` `super.Method()` | `super` references the base class | `No matching symbol 'super'`. `super::Method()` likewise. Only **`super(args)` inside a constructor** compiles; the idiom for a base method is `Base::F()`. | Our diagnostic on `super.F()` was right all along. Only the constructor form was a false positive, and it is fixed. `doc_p01`, `doc_r01` |
| `PROP-05` `property` on interfaces | valid on interface method declarations | `Expected ';' / Instead found identifier 'property'`, with and without `const` | `as-err-interface-method-attribute` (`rules/FunctionRules.cpp`) is correct as written. `doc_r02` |
| `PARSER-02` multiline `"..."` | a plain string may span newlines | `Multiline strings are not allowed in this application` — an engine property, off by default | The hand-rolled scanners in `FormattingHandler`, `PreprocessorRegions` and `IncludeResolver` that end a `"` string at the newline match the default engine. `"""…"""` heredoc **is** accepted and already parses. `doc_r03`, `doc_p12` |
| `PARSER-07` `class @Name {}` | declares a pure reference class | `Expected identifier / Instead found '@'` | Not a grammar gap. `doc_r04` |
| `TYPE-05` `byte` == `uint8` | `byte` is a built-in alias | `Identifier 'byte' is not a data type` | `byte` is host-registered. The real defect it gestures at is narrower — see the typedef entry under WIP. `doc_r05` |
| `TYPE-07` `opImplConv` → `bool` | makes `if (h && true)` work | `No conversion from 'H&' to 'bool' available.` | `as-err-ref-type-bool-conv-disallowed` is defensible. `doc_r06` |
| `PROP-01` automatic accessors | `get_X`/`set_X` are always the property `X` | `'V' is not a member of 'C'` **without** the `property` keyword; accepted with it | Engine-configurable (`asEP_PROPERTY_ACCESSOR_MODE`, SDK default 3). Our unconditional leniency misses errors but never invents them, so mode 2 stays the default and mode 3 is a setting every accessor rule now honours. `doc_r07`, `doc_p04` |
| `PROP-07` abstract classes | need not implement their interface | `Missing implementation of 'void I::P()'` | Inverted: `rules/ClassRules.cpp` skipped this check for abstract classes, which was a **missed** diagnostic. Fixed, and the same skip for mixins turned out to be wrong too. `doc_r08`, `doc_r23`, `doc_p21` |
| `PREDEF-02` `#if`/`#else` | selects the live branch | With the word undefined, **both** branches were dropped and the symbol was unresolved — CScriptBuilder has no `#else` | `utils/PreprocessorRegions.cpp` models exactly this already. Adding `#else` would diverge from the host that actually compiles these scripts. |
| `PARSER-04` UTF-8 BOM | breaks the lexer | accepted by the compiler — and by tree-sitter, because the grammar's `extras` is `/\s+/` and JavaScript's `\s` matches U+FEFF | Not a defect here. The only residue is that line 0 starts three bytes in. `doc_p14` |

---

## Confirmed correct, now guarded

Accepted by the compiler and by this analyzer. They had no test before; they do now.

| Document item | Script |
|---|---|
| `PARSER-01` leading-dot float literals `.30f` `.5` `.001` | `doc_p05` |
| `PARSER-06` `Type name(args);` at file scope is a variable, not a function | `doc_p11` |
| `PARSER-10` `T[]` carries `array<T>`'s methods | `doc_p09` |
| `PARSER-12` `property` / `override` / `final` / `explicit` as ordinary identifiers | `doc_p06` |
| `TYPE-08` `opIndex`'s element type surviving a member chain | `doc_p10` |
| `TYPE-11` `opMul_r` on the right operand | `doc_p07` |
| `LAMBDA-05` a funcdef selecting among overloads of `@Log` | `doc_p08` |
| `PROP-01` `get_`/`set_` with the `property` keyword | `doc_p04` |
| `PROP-02` interface accessor methods | `doc_p13` |

---

## Fixed

Each entry says what the compiler answers and what this analyzer answered before. Several were
false positives — legal code reported as an error, which is the one failure mode this project
treats as unacceptable.

- **Definite assignment: 749 findings over the corpus, and the rule was the wrong rule.** Found by
  giving `as-err-uninitialized-variable-read` a corpus audit, which it had never had. Five separate
  causes, each measured against the compiler rather than reasoned about:

  - **It was an ERROR and the compiler says WARNING.** Seven shapes measured, every one accepted
    with `WARNING: 'n' is not initialized.` and exit 0 — a plain read, a read inside an expression,
    a by-value argument, a `const &in` argument. Errors are what the parity audit counts and what a
    build gate stops on, so the severity was not cosmetic. Renamed to
    `as-warn-uninitialized-variable-read`.
  - **The rule was C#'s, not AngelScript's.** It implemented "definitely assigned on every path",
    intersecting branch states at every join. AngelScript warns only when **no assignment precedes
    the read at all**, conditional or not — `if (false) { x = 5; } Print(x);` is clean, and so is
    every loop form. Measured on ten shapes. The joins now union.
  - **A loop body's assignments did not survive the loop** unless the loop was provably infinite,
    which is the same C# reading. `for (...) { x = i; } Print(x);` is clean to the compiler.
  - **The conditions of `if`, `while`, `do`/`while` and `switch` were never analysed at all.**
    None of them carries a `condition` field in the grammar — the expression is an unnamed child —
    and the checker asked for the field, got null, and skipped it every time. Both halves were
    wrong: a read inside a condition went unchecked, and an `&out` argument written there never
    marked its variable assigned, which then reported the body. `for` was the same story under a
    different name: its field is `init`, not `initializer`, so `for (i = 0; i < n; i++)` reported
    the `i` in its own header.
  - **An argument to an invisible callee was read as a read.** Whether that parameter is `&out`
    is exactly what cannot be established, and `&out` is AngelScript's only way to return a second
    value, so it is what a bare local passed to an unknown function usually is. It is now treated
    as possibly written — the same ignorance runs both ways, so the variable is marked assigned
    rather than merely unread. Only a bare identifier: `f(x + 1)` cannot be an out-argument, so a
    compound expression stays judged.

  **749 → 7**, against the compiler's own 7 over the same files. Exactly one is the same finding.
  The other six of ours are all `value.Get(fvalue, strict)` in the JSON library, where the
  receiver's type IS visible and the out-parameter should have been recognised — and that is the
  same overload set `ResolveBestOverload` calls ambiguous 75 times, so the two are one defect. The
  six the compiler finds and this analyzer does not are misses, which is the safe direction.

  Six test assertions asserted the C# answer and have been inverted, each with the compiler's reply
  recorded beside it. One of them argued the point explicitly — "a standard for loop with condition
  i < 10 might execute 0 times, so x is unassigned" — which is exactly right about C# and wrong
  about AngelScript.

- **An `&out` parameter of a METHOD initialises its argument.** Found by `doc_p28`: the analyzer
  reported `int n; reader.Get(n, strict);` as using `n` uninitialised, which is the line that
  initialises it. The out-parameter rule existed and worked for free functions, whose candidate
  lookup takes the call node; the method branch resolved the receiver's type against
  `m_request.scopeRoot`, and a local receiver is never in the root scope. Now resolved in the scope
  the call is written in. **Measured effect on the corpus: none** — 749 both with and without it,
  because the corpus's method receivers are host types the analyzer cannot see at all, so the
  lookup fails for a different reason. It is proven by its unit tests and by `doc_p28`, where the
  class is declared in the script, and that is the honest extent of the claim.

- **`bool` is not a number, and three tables said it was.** It was listed as convertible to and from
  every numeric type in `OverloadResolver`'s `IsPrimitiveWidening` and `IsPrimitiveNarrowing`, and
  in `TypeConversionChecker`'s `IsConvertible`. The compiler allows none of it. Measured across the
  full matrix before anything changed — {`bool` → T, T → `bool`} × {argument, initializer} over all
  ten numeric types, **forty combinations, forty rejections** — and the forms outside the matrix
  agree: `int(b)` and `bool(n)` are "No conversion available", `b + 1` is "No conversion from 'bool'
  to math type available", `return b` from an `int` function and `b == n` are refusals, and `if (n)`
  is "Expression must be of boolean type". Narrowing was the worse of the two entries: narrowing is
  a *penalty*, which means viable, so the pairing scored as a candidate rather than being refused.

  `bool` is now refused explicitly in all three places rather than merely absent, with the
  measurement beside it, because the entry that was there looked deliberate. `string s = b;` stays
  legal — the string add-on really does register that `opAssign`, and it is the sink below the
  rejection rather than through it. `doc_r27`, `doc_p28`.

  **The hypothesis about what it was costing was wrong, and the measurement is what says so.** This
  item was filed claiming the 75 `as-err-call-ambiguous` findings over the corpus came from it:
  with `bool` convertible, `Get(bool&out, bool)` and `Get(int&out, bool)` would tie on an `int`
  argument. Correcting the tables **did not move that number at all** — the call-argument audit
  reads 103 before and after. Two things are now known that were not: the mutable-reference path in
  `ScoreArgumentMatch` already refused an `int` against a `bool&out` independently, so that overload
  set was never the tie; and `HasSameSignature` in `ResolveBestOverload` already handles one
  function arriving twice, so two copies of one library are not it either. The 75 are a genuine tie
  between two *different* signatures, they have not been reproduced outside the corpus — three
  hand-written versions including the namespaced spelling all resolve cleanly — and they are now
  recorded as **not diagnosed** rather than explained. That is the next item.

  What the fix did do, measured: the type-conversion audit moved **25 → 27**, and every one of the
  three findings that appeared was worth having. Two are true positives the analyzer had been unable
  to see — `int InSquad() { return m_hSquadLeader != NULL; }` returns a bool from an int function,
  which the compiler refuses. The third was a false positive, fixed rather than counted, below.

- **`T[] name(size)` was read as converting the size into one element.** Found by the entry above:
  `bool[] g_playerGlowEnable(32+1);` was reported as "Cannot implicitly convert 'int' to 'bool'".
  The bracket spelling reduces to its ELEMENT type in both places that judge a direct
  initialization, so the size looked like an initializer for one `T`. It had been silent since
  forever because `int[] a(33)` and `float[] a(33)` are the same misreading and `int -> int` and
  `int -> float` score fine — correcting `bool` is what made it visible. Guarded now in
  `ReadDeclaredType`, where the written SHAPE is tested before anything about the base name, and in
  `CallChecker::CheckVariableDirectInitialization`. The angle spelling never needed a guard:
  `array<int>` keeps `array` as its container name, which is not a primitive, so it already took
  the class path.


- **The corpus audits run in CI, in parallel.** They share nothing and are single-threaded, so the
  workflow now runs one process per audit at `nproc` at a time: measured locally, 19 of them took
  about 40 minutes in series and about 7 in parallel. One process each also buys what a single
  `--test-case="*Corpus Audit*"` run cannot — a killed or timed-out job leaves the finished audits'
  logs behind, where one buffered process leaves nothing and a truncated run looks exactly like a
  passing one. That is not hypothetical: it happened here, and the empty log was read as success
  until the missing `Status:` line gave it away. The step now fails on an audit that reported
  nothing, not only on one that reported a finding, and uploads every log whatever the outcome.

  Still worth doing: the 21 audits repeat the same walk — 1,061 files parsed, symbol table built,
  the whole analyzer run — 21 times, to keep one diagnostic code each. One pass counting every code
  would be roughly 21× less work. It is a change to 21 test cases and their ratchets, so it is its
  own piece of work rather than a passenger in this one.
- **A lambda against its target funcdef.** `CheckFuncdefAssignment` handled only a named function
  on the right-hand side; a lambda fell through the symbol lookup, found nothing and returned in
  silence, so neither arity nor parameter types were compared. The compiler compares the **written**
  signature and does not convert it, which makes the accepting cases the hard part:
  - **Arity is a hard equality**, even when every parameter is untyped, and **a funcdef's default
    argument does not relax it**: `funcdef void CB(int a = 1)` still rejects `function() { }`. A
    default argument is for calls, not for the shape of the handle.
  - **A written parameter type does not widen.** `funcdef void CB(int)` rejects `function(uint a)`.
  - **The decorations are part of the signature.** `const string &in` rejects `string`, rejects
    `string &in`, and `Foo@` rejects `Foo` — each with its own error.
  - **An omitted type is a wildcard**, which is the point of the feature: the type comes from the
    funcdef, and a partly-typed list like `function(int a, b)` is legal.

  The false-positive surface is the type NAME, because the compiler resolves it and this rule only
  reads it. All of these are accepted, and a string comparison reports every one: a `typedef float
  real` against `float`, `array<int>@` against `int[]@`, and a namespaced type written bare inside
  its namespace and qualified outside. So the name is compared by its last `::` segment and not at
  all when either side names a typedef — the one alias no spelling can see through. `array<int>`
  and `int[]` need no case of their own, since `CleanBaseType` reduces both to the element type.
  The decorations, by contrast, are compared whatever the name is: a typedef aliases primitives
  only (see `as-err-typedef-non-primitive`) and a namespace qualifies a name without changing
  whether it is a handle, so no spelling can hide one.

  The corpus decided the scope. It writes 33 lambdas across 17 files and **not one** of them is
  `CB@ cb = function(...)`; every one is a call argument — `arrOut.sort(function(a, b){})`,
  `Hooks.RegisterHook(..., @MapChangeHook(function(...)))`. So the funcdef *conversion* form
  `MapChangeHook( function(...) )` was implemented alongside the assignment, since the compiler
  judges it by the same rules — `No matching signatures to 'CB(<auto> lambda())'` — and it is what
  real code writes. **Still a gap:** a lambda passed straight to an ordinary function whose
  parameter is a funcdef handle. That is `CallChecker`'s overload resolution rather than this rule,
  with its own false-positive surface, and it is where the remaining corpus lambdas live.

  Verified against the compiler position by position: over `tests/parity/`, the analyzer reports
  **12 of the compiler's 12** rejections in `doc_r26`, on the same lines, and **nothing at all** in
  `doc_p27`, which collects every accepting shape including the three spelling traps. The corpus
  audit reads 0, but that number is a guard rather than a measurement and says so at its own site:
  the funcdefs the corpus's lambdas name are registered by the game engine in C++ and declared in no
  script, so the rule stays silent there by the visibility policy rather than by judging anything.
  `doc_p27`, `doc_r26`.

- **Every engine property the analyzer models is now askable of the oracle.** `angelscript_oracle`
  could set only `asEP_PROPERTY_ACCESSOR_MODE`, so the other three members of
  `config::EngineProperties` had their behaviour recorded on faith — the same defect that had left
  half of `doc_r07` unmeasured. Added `--allow-unsafe-references`, `--private-prop-as-protected`
  and `--disallow-global-vars`, each `-1`-by-default so every existing invocation answers exactly
  as it did. All three were then confirmed to flip a verdict: `void f(int &x)` goes from
  "Only object types that support object handles can use &inout" to accepted, a global variable
  from accepted to "Global variables have been disabled by the application", and inherited private
  access from an error to `WARNING: Accessing private property 'm' of parent class` — a warning
  this analyzer does not yet emit under that setting, which is a gap the flag has now made
  visible rather than one it created.
- **Numeric warnings** (`TYPE-03`). The compiler emits **five**, not the three the issue list named,
  and only two of the five are decidable from types. The other three — `Implicit conversion changed
  sign of value`, `Value is too large for data type`, `Implicit conversion of value is not exact` —
  fire only on *constant* expressions and need a constant folder, so they are deliberately not
  implemented rather than approximated. The two that landed, both in `TypeConversionChecker`:
  - `as-warn-signed-unsigned-mismatch` fires **only on the six comparison operators**. The issue
    list implied it followed the conversion; the oracle says `i * u`, `i & u`, assignment, argument
    passing and return are all silent. Width is irrelevant — every signed integer against every
    unsigned one warns — and **float and double count as signed**: `float < uint` warns, `float <
    int` does not.
  - `as-warn-float-truncation` fires wherever a float value implicitly reaches an integer slot:
    initializer, assignment, compound assignment, return.

  The whole false-positive surface is constant folding, and it is not hypothetical. **A compile-time
  constant on either side is folded and judged by value, not by type**, so the compiler is silent
  where a type-only rule would speak: `const int i = 1; i < u` is clean, a bare enum member is
  clean, and `const float D = 15.0; int m = D;` is clean because 15.0 survives the trip exactly —
  while `const float D = 15.5` is answered by `Implicit conversion of value is not exact`, a
  different code. Both rules therefore stay silent whenever the operand is constant. That costs
  `u < -5`, which the compiler does warn about; missing beats inventing, and the alternative is the
  folder again.

  The first run of the new corpus audit reported **35 findings, every one a false positive**: 34 of
  the `const float WEAPON_DAMAGE = 15.0; int m_iBulletDamage = WEAPON_DAMAGE;` shape across the
  Sven Co-op weapon scripts, and one on a Mersenne twister — `y ^= (y << 15) & 0xefc60000;` with
  `uint64 y`, reported as a truncation into an integer. That last one was **a pre-existing defect in
  `ResolveExpressionType`, not in the new rule**: it scanned every number literal for the float
  suffix and the exponent marker, and `d`, `e` and `f` are hex digits, so `0xefc60000` resolved to
  `float` for every rule downstream. Hex and binary literals are now excluded from that scan; `u`
  and `l` are not hex digits, so the width suffixes still read.

  `IsPrimitiveWidening` was **not** reused, and the note at its site now says why: it lists
  signed/unsigned pairs as *safe* on purpose, because removing them produced real false positives on
  `array<int> a(1)`. The compiler warns on exactly the pairs that table calls safe. Two questions,
  two tables.

  Cross-checked by running `angelscript_oracle` over all 1,061 corpus files and counting its own
  numeric warnings: **compiler 2 and 0, analyzer 0 and 0** — nothing invented, two missed, both the
  same `key[0] == '*'` on a `string`, where the audit loads no stubs and cannot see that `opIndex`
  returns `uint8`. Real AngelScript writes almost none of this: 541 of the corpus's indexed loops
  declare `uint i` against 10 that declare `int`, and all 10 of those bound on `ArgC()`, which
  returns `int` - so the compiler has nothing to warn about either. The rules
  are exercised for real by the parity files, where the analyzer matches the compiler **line and
  column** on 10 of 10 mismatches and 8 of 9 truncations. `doc_p25`, `doc_p26` (which states the
  ninth: `int i = f * 2.0f;`, missed because the initializer path resolves through
  `ResolveValueType`, which has no binary-expression case).
- **`super(args)` in a constructor.** `NamespaceChecker` emitted `as-err-undefined-identifier` and
  `SemanticAnalyzer` a matching warning, because `super` resolves to no symbol and never will. Now
  `SemanticHelpers::IsBaseConstructorCall` tests the *shape* — a constructor of a class with a base
  list — so `super.F()`, `super::F()` and `super(...)` in a class with no base are still reported.
  `doc_p01`, `doc_r01`.
- **`int` → `enum`.** `TypeConversionChecker` allowed the implicit conversion where the compiler
  rejects it (`Can't implicitly convert from 'int' to 'Color'`), and `OverloadResolver` had always
  agreed with the compiler - so `SetMode(1)` was reported and the identical mistake in an assignment
  was not. An enum is now a sink in `IsConvertible`: nothing reaches it implicitly but itself, while
  widening out of it, the explicit `Color(1)` cast and a class declaring an operator that produces
  one all stay legal. A test asserted the old, wrong answer under the heading "enums are out of
  scope"; it now asserts the compiler's. `doc_r09`, `doc_p15`
- **Metadata blocks and omitted initializer elements.** `[Property, Category="Weapons"]` before a
  declaration and `{ 0, 1, , 4, 5 }` both compile — `CScriptBuilder` strips the metadata before the
  compiler sees it, and an omitted element takes the type's default. Neither had a grammar rule, so
  each turned its whole declaration into an ERROR node and the symbol left the index along with the
  annotation. Fixed in `tree-sitter-angelscript` on `feat/metadata-and-list-holes` (corpus
  181 → 188): metadata is a sibling of the declaration it precedes, the way the builder treats it,
  and the initializer list is written with the comma as the anchor so `{ }` keeps a single
  derivation. `doc_g02`, `doc_g03`.

  **Shipped.** `server/cmake/TreeSitter.cmake` pins `aa14847`, and the two `KnownGaps()` entries
  in `ParityAuditTest.cpp` are gone with it. The audit could never have held them anyway - it fails
  only on a false positive, and an ERROR node costs a symbol rather than producing a diagnostic, so
  both gaps read to it as silence. The guards that replace them are in `SymbolCollectorTest.cpp`:
  each construct parses without an ERROR node and its symbol is still in the index.
- **Calls through a `using namespace`, and a fabricated ambiguity rule.** A using-directive puts a
  name in reach and the compiler judges a call through one like any other — `using namespace A;
  f(id)` with `int` against `A::f(string)` is `No matching signatures to 'f(int)'`. Candidate lookup
  skipped directives entirely. It now falls through to the imported namespaces when no lexical scope
  declares the name, and *merges* all of them rather than stopping at the first, which is what the
  compiler does. Lexical scopes still shadow: `N::f(string)` hides the global `f(int)` from inside
  `N`.

  Writing that merge turned up `as-err-ambiguous-identifier`, raised whenever more than one
  using-directive declared a name. The compiler merges and lets overload resolution choose, and says
  `Multiple matching signatures` only when resolution itself cannot; two namespaces declaring the
  same *variable* it does not report at all. The rule counted scopes and called that a verdict — the
  same shape as the fabricated `as-err-unary-neg-on-unsigned` the earlier parity work removed — so
  it is deleted. The verdict it stood in for now arrives honestly, as `as-err-call-ambiguous` from
  overload resolution. `doc_r16`, `doc_r17`, `doc_p16`, `doc_p17`.
- **Calls inside a namespace, unchecked entirely.** Reported from the field: `my_test_func(id)`
  with an `int` argument and a `string` parameter drew nothing, while the identical call at file
  scope was reported. `FindFreeCandidates` probed the symbol table for the unqualified spelling
  only, and the collector keys a namespaced function under its qualified name alone —
  `TEST::my_test_func`, never `my_test_func` — so the candidate set came back empty and *every*
  call in the file went unjudged: counts, types, ambiguity, all of it. Because "no visible
  declaration" is also what an engine function looks like, nothing about it looked wrong from the
  inside. It now walks the scopes an unqualified name may name — innermost namespace, each
  enclosing one, then global — stopping at the first that declares the name. `doc_r12`, `doc_r14`,
  `doc_r15`.
- **A list with no pattern said nothing about itself.** A list factory is registered in C++ and no
  stub can express it, so a type with no `/// @listpattern` tag has its list left entirely
  unchecked — shape and contents both. Silence is the right verdict and a useless explanation. There
  is now a Hint, `as-hint-list-pattern-unknown`, on the list itself, naming the type and the tag to
  add. Only where it can be acted on: an engine-registered type has no declaration to tag, and a
  primitive is still an error rather than a suggestion, since its silence in a stub proves
  something. A Hint and not a warning — the code compiles, and this says the analyzer is missing
  something, never that the script is.
- **Unknown parameter and return types, now on by default.** `void f(TypoTypeName x)` is a compile
  error, and left unreported it surfaced as silence at *every call site*, since a call whose
  parameter types are unknown cannot be judged either — the cost was a whole function's worth of
  checking, not one diagnostic. `angelscript.diagnostics.reportUnknownTypes` defaults to `true`; a
  workspace whose host registers types in C++ and declares none of them sets it to `false`, or
  better, names its engine profile. Measured on the 1061-file corpus with the Sven Co-op profile
  loaded: 1581 findings off, 3850 on. `doc_r13`.
- **Initializer-list element types.** `array<int> a = {"x"}` was silent where the compiler answers
  `Can't implicitly convert from 'const string' to 'int&'`. The list's shape was checked and its
  contents were not, because the conversion pass skips initializer lists outright.
  `CanConvertImplicitly` is now exported from that pass and `InitializerListChecker` borrows the
  judgement — and with it the silence, since an unresolved element or target comes back
  convertible. `doc_r10`.
- **A bare accessor name inside a method.** `class C { int get_Up() const property { … } void T()
  { int v = Up; } }` compiles, and no symbol is ever named `Up` - the member is `C::get_Up` - so
  every use of a virtual property from inside its own class drew `as-err-undeclared-identifier`.
  `RuleIndex` now records the property name behind each `get_`/`set_` member, in two sets so the
  reader can pick by accessor mode. `doc_p03`, `doc_r07`.
- **`case Red:` on an enum member.** Enum members were collected as ordinary `Variable` symbols and
  nothing set `isConst`, so the switch rule read them as mutable and drew
  `as-err-case-not-constant`. The compiler's own answer to `Red = 5;` is "Expression is not an
  l-value", so the constness is now recorded at collection where every rule can see it. Found by
  `doc_p15`, which was written for the enum-conversion work below and turned this up on the way.
- **A lambda reading a global.** `AccessChecker` enforces AngelScript's no-closure rule by resolving
  the name in the scope tree, and `LOCALS_QUERY` records a module-level global under the same
  `LocalDefinitionKind::Variable` as a function-body local. Every legal global read inside
  `function(){}` was reported. `ResolveInScope` now has an overload reporting the owning scope, and
  the rule requires `Scope::isFunctionScope` somewhere in that scope's chain. `doc_p02`, `doc_r11`.
- **The client had no failure surface.** `extension.ts` caught a start failure, wrote one line to
  an output channel and returned, so a server that never started and a server working normally
  looked identical from the editor - no diagnostics either way, and the only difference in a panel
  nobody has open. Three things now report it. A missing binary is checked *before* the spawn, since
  it is the one failure with a specific cause - no build for this platform, or a source checkout
  that was never built - and arriving as a bare ENOENT threw that away; the message names the
  platform and the log lists every path tried. A start failure raises `showErrorMessage` with a
  button that opens the log. And an `errorHandler` reports the case the language client's default
  handles by giving up quietly: after four unexpected exits it stops restarting, which used to be
  indistinguishable from a server with nothing to say. A status bar item carries the state the rest
  of the time, and `AngelScript: Show Server Log` reaches the channel from the palette.
- **A typedef inside a template argument.** `TYPE-05` in its true, narrow form, and the only item
  on the WIP list that reported legal code. A typedef names a primitive and the name *is* that type
  inside a template argument as much as anywhere else: `typedef uint8 byte;` makes `array<byte>`
  and `array<uint8>` one instantiation, and the compiler accepts a call and an assignment between
  them in either direction. `OverloadResolver::UnwrapTypedef` unwrapped the outer name only, so the
  two were unrelated spellings, no candidate scored, and `Take(alias)` drew
  `as-err-no-implicit-conversion` — a false positive. It now descends into the argument list,
  splitting on top-level commas so `dictionary<string, array<int>>` stays two arguments and not
  three, and rebuilding as it goes, which canonicalises the separator too: `array<int,string>` and
  `array<int, string>` had been different types to a string comparison. The assignment direction
  was already silent and now has a guard beside it. `doc_p19`.
- **Initializer lists, the other five positions and the count.** A list was visited only on a
  `variable_declaration`. The grammar allows one under six parents, and the compiler accepts every
  one of them - it infers the target type from the parameter, the assignee or the declared return
  type and compiles the list against that - so `Take({"x"})`, `a = {"x"}` and `return {1,"x"};` all
  went unjudged where the compiler answers `Can't implicitly convert from 'const string' to 'int&'`.
  All six are now reached: the declarator, the assignment (plain `=` only - `a += {1}` is "Illegal
  operation on 'int[]&'", a verdict about the operator), the return, a nested list, the `array<int>
  = {…}` anonymous object, and the call argument. The argument case is driven from `CallChecker`,
  which is the pass that knows which overload was picked; it validates only where the pick is not in
  question - one candidate of that arity and no named arguments - because with two the compiler's
  own answer is `Multiple matching signatures to 'Take({...})'`. `doc_r18`, `doc_r19`, `doc_r20`,
  `doc_p18`.

  The element *count* is checked too, but only against a group with no `repeat` in it: a repeat
  consumes everything from its position onward and is satisfied by none at all, so `array<int> a =
  {};` compiles. A fixed group is exact in both directions - `dictionary`'s `{repeat {string, ?}}`
  gives `Not enough values to match pattern` for `{{'a'}}` and `Too many values` for `{{'a',1,2}}` -
  which is `as-err-initializer-list-too-few` and `-too-many`. The count comes from the *separators*,
  never from the nodes: an omitted element produces no node and the compiler still counts it, and
  `dictionary d = {{'a',}};` compiles, so counting children would have reported legal code.
  `doc_r21`, `doc_r22`, and `doc_g03` for the hole.

  Three corpus cases stay missed for the visibility policy rather than for a defect: `il_a4`
  (`array<array<int>> g = {1,2}` — the element type is a template, which does not resolve), `il_a6`
  and `il_a7` (`string` declares no `@listpattern`, and an absent pattern only means the stub did
  not say).
- **`asEP_PROPERTY_ACCESSOR_MODE` under mode 3.** The setting existed, was documented, and exactly
  one rule read it — so `propertyAccessorMode = 3` changed the undeclared-identifier check and
  nothing else. A `c.V` whose accessor lacks the `property` keyword was still accepted, where a
  mode-3 engine answers `'V' is not a member of 'C'`. A setting honoured in the one place that
  remembered to is worse than no setting: it reads as supported.

  `AccessChecker::FindMember` treated `get_X`/`set_X` as the member `X` unconditionally. It now
  asks, and so does the rule that already did, through one shared
  `SemanticAnalysisRequest::RequiresAccessorKeyword()` rather than the mode number spelled out at
  each site.

  **The oracle grew a `--property-accessor-mode` flag for this.** It had always used the SDK
  default of 3, so the mode-2 half of every answer was unaskable — recorded on faith. Now both
  halves are measured, and `doc_r07` carries them:

  ```
  angelscript_oracle doc_r07_accessor_without_kw.as --property-accessor-mode=3   'V' is not a member of 'C'
  angelscript_oracle doc_r07_accessor_without_kw.as --property-accessor-mode=2   accepted
  ```

  The parity audit runs without the flag, so `doc_r07` is judged under the default and stays a
  `doc_r`; the mode-2 half is asserted in `AccessCheckerTest`, because a `doc_p` file that only
  compiles under a non-default engine property would fail the audit for a reason that has nothing
  to do with the analyzer. Mode 2 remains this server's default: under 2 it misses a diagnostic a
  mode-3 host would give, under 3 it would invent one for a mode-2 host, and missing beats
  inventing.
- **The 273 conversion false positives, down to 25 — and none of the 25 is a defect.** The first
  thing the corpus audits reported once they ran. Every corpus file is working AngelScript, so
  every finding was legal code called an error. Six causes accounted for 248 of them:

  | Cause | Findings | Answer from |
  |---|---|---|
  | **`string` is a sink.** `IsConvertible` had `string` in its built-in set and then returned `false` for every non-numeric pair, so `string s = i;` was an error. The add-on registers an `opAssign` for every scalar — measured one type at a time — and `"" + x` asks the same question. | 149 | `doc_p23`, `doc_r25` |
  | **Constructing a type whose constructors are the host's.** `string(count)`, `EHandle(h)`, `Vector(x)`. `CheckConstruction` walked the declared constructors and reported when it found none, without ever asking whether it could see any. Its sibling `CheckDefaultConstructor` had that guard already. | ~60 | the visibility policy |
  | **Engine class handles.** `CBasePlayer@` where `CBaseEntity@` is expected is an upcast, and the hierarchy that makes it one is written in C++. The overload scorer walked a hierarchy that stops at the name and scored "found no relation" the same as "incompatible". Now `OverloadMatchPenalty::UnknownTypes`, ranked last among the viable scores so it never displaces a match that *is* visible. | 13 | the visibility policy |
  | **An enum widening out to an integer.** `Take(ModeOne)` against `void Take(int)`. Only the inward direction is closed — `Color c = 1;` is still the error. | 11 | `doc_p24` |
  | **A namespaced class's constructor.** `Hook`'s constructor is keyed `Hooks::Hook::Hook`, and both keys built from the written spelling — `Hook::Hook` — reach nothing. The *class* was found, because that lookup already falls back to the last segment, so the pass concluded it had no constructors at all. | 10 | `doc_p24` |
  | **`auto`, in three separate places.** Not a type: a placeholder for what the initializer produces. It was in `IsCorePrimitive`'s list, so `auto@` was a handle on a primitive; it scored as incompatible in overload resolution; and it was judged as a conversion source. | 7 | `doc_p22`, `doc_p24` |
  | **`array<T> a(33)`.** The count goes to the container's initial-size constructor, and `CleanBaseType` had already reduced the declared type to its *element*, so the question asked was whether an `int` can become a `PlayerSlide`. | 3 | `doc_p24` |

  The 25 that remain are read and accounted for. **Nineteen are true positives**:
  `AngelScripts_SteamIDHelper.as` passes an `int64` and a `STEAMID_FLAG` to `void println(string)`,
  and the compiler rejects both — argument passing does not go through `opAssign` the way an
  assignment does, which is the one asymmetry that makes `string s = v;` legal and `Take(v)` not.
  Three more are `angelscript_clean_examples.as` declaring `class A` and `class B` twice with
  different bases; it is a documentation dump rather than a module. The last three are an
  eight-overload `ToArray` set across two namespace versions, not yet diagnosed.

  `optional.as` came out of `KnownGaps()` on its own. It had been recorded as a stub gap —
  `optional<T>`'s constructor is registered in C++ and no stub declares it — and it was never that:
  it was the analyzer answering a question it could not see the evidence for, which is the same
  mistake the 273 were, at one file's scale.
- **The corpus audits, which had never run in CI.** Nineteen test cases walk the ~1,061-file
  `angelscript/` corpus and ask the only question this project treats as fatal — does any rule
  report code that compiles? Every one is `skip()`-decorated, so `ctest` passes them over, and the
  workflow ran none. They executed only when someone remembered the incantation.

  Two things had to be true before CI could run them. First, the absence of the corpus had to be a
  clean skip: `fs::directory_iterator` with no `error_code` **throws**, so in CI all nineteen would
  have crashed rather than skipped. `tests/helpers/CorpusDirectory.h` resolves the directory once —
  `ANGELLSP_CORPUS_DIR`, else the compiled-in path, else empty — and an override that was set but
  does not resolve reports *absent* rather than falling back, so a failed checkout cannot quietly
  audit a different tree and call it a pass. Second, the corpus is 13 MB of third-party scripts and
  `.gitignore` has excluded it since the first commit, so `.github/workflows/corpus-audit.yml`
  fetches it from a repository named by the `CORPUS_REPO` variable and, without one, reports that
  it measured nothing. Weekly plus `workflow_dispatch`, on a **Release** build: one audit is 80
  seconds optimised and about twenty minutes without.

  Running them found three failing, all of it drift that predates this work — verified by building
  `37c2dee` in a worktree and getting the identical numbers:

  | Audit | Before | After |
  |---|---|---|
  | `TypeRules` | 3 | **1** |
  | `VariableRules` | 3 | **1** |
  | `TypeConversion` | 273 | 273, now a documented ratchet |

  The two that are fixed were one false positive: **`auto@` was reported as a handle on a
  primitive.** `IsCorePrimitive` lists `auto` beside `int` and `float`, and `auto` is not a
  primitive — it is not a type at all, but a placeholder for whatever the initializer produces, so
  whether a handle is allowed is decided by *that* type. The compiler accepts
  `auto@ g = MakeFoo();` and rejects `int@ x;`; this reported both. Two corpus scripts declare a
  deduced handle and both were flagged. `doc_p22`, `doc_r24`.

  `TypeConversion`'s 273 stay open and are now asserted as a ceiling that may only fall, so the job
  gates against regression while the causes are found. The largest known one:
  `array<float> a(33);` draws `No conversion from 'int' to 'float'` — the initial-size constructor
  takes a `uint` count and the argument is being checked against the *element* type instead. See
  the WIP list.
- **Array members, in both spellings.** `int[]` and `array<int>` are one type, and neither reached
  member resolution as one. `CleanBaseType` answers the *element* type — it reduces both to `int` —
  which is right for the question it is asked almost everywhere and wrong for "what type owns this
  member", and the two were the same call. `a.length()` looked for `int::length`, found nothing,
  and `CallChecker`'s visibility guard then sent every call on an array away unchecked.

  Recorded in the backlog as a bracket-only defect, and it was not: the template spelling was
  broken in the same place. It looked healthy because `length`, `size` and `isEmpty` had a
  hardcoded shortcut in the call-expression branch — a fourth method has no shortcut and produced
  nothing for either spelling. Two functions now say what was being conflated:
  `CanonicalizeArrayType` (`int[]` → `array<int>`, innermost first so `int[][]` is
  `array<array<int>>`) and `MemberOwnerType` (either spelling → `array`). `CompletionHandler` had
  a private copy of the first, which is exactly why completion worked on `int[]` while hover and
  call checking did not; it now uses the shared one. The container name is read from
  `TypeConfig::arrayTypeName` where the config is in reach, and defaults to `"array"` where it is
  not — which is what every hardcoded literal there was already assuming.
- **Hover showed the wrong file's documentation comment.** A doc comment sits above the
  *declaration*, and a symbol's `startLine` counts lines in the file that declares it — but the
  handler read `request.sourceCode`, the file being hovered over. Hovering a symbol declared at
  line 12 of another file rendered whatever was at line 12 *here*, presented as that symbol's
  documentation. Wrong documentation is worse than none, and it fails silently: it looks like a
  working feature.

  Two of the four sites were affected; the other two resolve a `LocalDefinition`, which is
  same-file by construction. The fix is the pattern `ResolveCompletionItem` already used — a
  `readDocument` reader on the request, supplied from the same place in `Server.cpp` — and with no
  reader the answer is no comment rather than a guess.

  `SUGG-04` lands with it, because it is the same function: a method with no comment of its own now
  shows the one on the declaration it implements. That is where the contract is written, and
  repeating it on every implementer is what nobody does. Only from an ancestor of the method's own
  container, so an unrelated method of the same name is never consulted.
- **Abstract classes and mixins must implement their interfaces.** `CheckInterfaceImplementation`
  opened with `if (sig.modifiers.isAbstract || sig.modifiers.isMixin) return;`, on the reading that
  either may leave the interface to whoever derives from it. The compiler disagrees with both
  halves, and the mixin half is the surprising one:

  | Case | Compiler |
  |---|---|
  | `abstract class A : I {}` | `Missing implementation of 'void I::P()'` |
  | `abstract class A : I { void P() {} }` | accepts |
  | `mixin class M : I {}` + `class C : M { void P() {} }` | **rejects, against `M`** |
  | `mixin class M : I { void P() {} }` + `class C : M {}` | accepts |

  A mixin that names an interface carries it itself; a class including the mixin and implementing
  the method does not satisfy the mixin. Two missed diagnostics, not a policy. `doc_r08`,
  `doc_r23`, `doc_p21`.

  The skip is replaced by the guard the rule never had. Every method it reports is one it did not
  find, so a base it cannot read is a base whose members it would report as missing — and that is
  exactly what an engine-registered class looks like from here. `HierarchyIsFullyVisible` answers
  that, and it existed already: **four identical copies**, one in each of `CallChecker`,
  `AccessChecker`, `ConstChecker` and `FunctionRules`, each in its own anonymous namespace. It is
  now declared once in `SemanticHelpers`.

  Two tests asserted the old answer and now assert the compiler's — `ClassRulesTest`'s "An abstract
  class may leave the interface to its subclasses", and `TypeRulesTest`'s mixin case, whose
  `// OK: Completes Stop() implementation` was the wrong half of the pair.
- **The formatter, which was rewriting working code.** Every `{` went onto its own line
  unconditionally, so `array<int> a = {1, 2, 3};` and every lambda passed as an argument were
  exploded Allman-style, `#if` was torn out of the block it delimits to column zero, and a metadata
  block was joined onto the declaration it annotates. A brace is now classified as *block* or
  *value*: a value brace stays on its line, costs no indent level, and is padded by what it holds -
  `{ Log(a); }` for a lambda body, `{1, 2}` for a list. The test is not `parenDepth > 0` but
  "deeper than the brace that encloses it", because `f(function() { if (c) { g(); } })` has both
  braces at `parenDepth == 1` and only one of them is a value. `angelscript.format.braceStyle`
  (`--format-brace-style=allman|kr`) moves the *block* brace and nothing else; Allman stays the
  default, which is what every existing test asserts.

  The measurement that mattered is in `tests/FormatterCorpusTest.cpp`: format every script, and
  check the token stream is unchanged and that formatting twice equals formatting once. Run against
  the corpus it found three ways the formatter had been changing what a file *means*, none of them
  from this work:

  - **`!isdigit(s)` came out as `!is digit(s)`.** `!is` is the handle-inequality operator and the
    only operator spelled with letters, so it is the only one that can swallow the front of an
    identifier; it matched greedily with no word-boundary test. **Twenty-eight of the 1061 corpus
    scripts** were being rewritten into something that does not compile, on every format.
    `doc_p20`.
  - **A UTF-8 BOM was split into three tokens** with spaces between them, so `doc_p14` - a file the
    compiler accepts - stopped compiling after formatting. The BOM is now held aside and put back
    byte for byte, and any run of non-ASCII bytes is kept whole.
  - **An unterminated string swallowed the next line.** `"` ends at the line break, matching the
    default engine, and the token left over ran into the end of its line; joining the following
    line onto it moved that code inside the literal. That is the state every string is in while it
    is being typed.

  Confirmed the way the rest of this file is: the oracle compiled all 87 parity scripts before and
  after formatting, and every verdict is identical. Over the full 1061-script corpus the token
  stream survives and formatting is idempotent for every file.
- **`angelscript.restartServer`.** Purely client-side, and the sequence already existed inside
  `onDidChangeConfiguration`: stop the client, reset the exit budget, build a new one. It is
  extracted as `restartClient` and both callers use it. No `workspace/executeCommand` - every
  setting that matters is a command-line argument captured when `ServerOptions` is constructed, so
  a new client *is* the restart, and a request handled by the process being restarted could not
  outlive it anyway.
- **Completion inside comments, strings and after a `case` label.** `.` and `:` are trigger
  characters and the global fallback answers whatever the earlier contexts do not claim, so typing
  the colon of `case Red:` - a position where no symbol may be written - returned every local,
  every global and all 60 keywords, and so did every character typed inside a comment or a string.
  `GetCompletion` now classifies the cursor before anything else: a scan from the top of the file
  says comment, string or code, and the first two are answered with nothing. Read by hand rather
  than off `request.tree`, because completion is asked for mid-edit, where an unterminated string
  and an unclosed block comment are ERROR nodes rather than string and comment ones - the exact
  states the suppression exists for. The scan follows the default engine, like the ones in
  `FormattingHandler` and `PreprocessorRegions`: `"` and `'` both end at the line break, so one
  missing quote cannot silence the rest of the file, while a `"""` heredoc spans lines. `case
  Some::` still completes - it ends in `::`, which is the qualifier context and not a finished
  label.

---

## WIP

Real, oracle-confirmed, and none of it reports legal code today. Ordered by what a user notices
first. Each lands with its `doc_`-prefixed parity case.

1. **One overload-resolution defect, two symptoms, not diagnosed.** 75 `as-err-call-ambiguous`
   findings over the corpus, and the six definite-assignment findings that remain after that rule
   was fixed - both on the same `Get` / `opAssign` / `opIndex` overload sets in a JSON library the
   corpus carries twice. The ambiguity half: Legal code reported as
   an ambiguous call, concentrated in a JSON library the corpus carries twice - `opAssign`,
   `opIndex`, `Get`, `Set`. The `bool` conversion tables were the first hypothesis and correcting
   them moved the number not at all, so that is ruled out and so are the two other obvious
   candidates: `ScoreArgumentMatch`'s mutable-reference path already refuses a converting argument
   against an `&out` parameter, and `HasSameSignature` already stops one function arriving twice
   from tying with itself. What is left is a genuine tie between two DIFFERENT signatures, and it
   has not been reproduced outside the corpus - three hand-written versions of the shape, including
   the namespaced spelling, all resolve cleanly. The next step is to instrument
   `ResolveBestOverload` over one of those files rather than to guess again. Pinned at 103 in
   `CallCheckerTest.cpp` so it can only go down.

2. **A way to know a workspace's stubs are complete.** `reportUnknownTypes` is a declaration by the
   user, not a deduction: nothing lets the server establish that an unresolved name is a typo rather
   than a host type. An engine profile is the closest thing, and it is a fixed list. Until that
   exists the setting is the honest interface, but it is the reason the rule cannot simply be
   unconditional.
3. **URI normalization** — `Server::CanonicalPathFromUri` and `UriFromPath` exist and are used at no
    request entry point; handlers key their maps on the raw string, so `file:///e%3A/…` and
    `file:///E%3A/…` are different documents on Windows.
4. **`as.predefined` hot reload** — the stub is re-parsed on change but the reload does not mark the
    graph dirty, so open documents keep stale diagnostics until the next keystroke.
5. **`workspace.fileOperations`** — `didRenameFiles` / `didDeleteFiles`, plus an `#include` fixup on
    rename. `WorkspaceIncludeGraph` already holds the graph the edit needs.
6. **Exclude globs and a project root** (`PREDEF-07`) — three unbounded recursive directory walks per
    workspace root, with no way to skip build output.
7. **Untested but confirmed correct** — a corpus case for the `Foo obj(bar);` most-vexing-parse,
    where `func_declaration`'s `prec.dynamic(2)` currently outranks `variable_declaration`'s `1`.

### Out of scope

`TOOL-04` (a Debug Adapter Protocol sidecar) is a separate program, not a language-server feature.
