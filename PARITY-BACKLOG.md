# Parity backlog

An issue list from a second AngelScript language server was distilled into ~60 items covering the
parser, the type system, scope, properties, lambdas, `as.predefined`, completion and the protocol.
This file records what happened when each claim was put to a real compiler, and what is left to
build.

The suite here passes at 1195 with zero unexplained false positives across six corpora, so nothing
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
| `PROP-01` automatic accessors | `get_X`/`set_X` are always the property `X` | `'V' is not a member of 'C'` **without** the `property` keyword; accepted with it | Engine-configurable (`asEP_PROPERTY_ACCESSOR_MODE`, SDK default 3). Our unconditional leniency misses errors but never invents them, so it stays the default and mode 3 becomes a setting - see WIP. `doc_r07`, `doc_p04` |
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

1. **A way to know a workspace's stubs are complete.** `reportUnknownTypes` is a declaration by the
   user, not a deduction: nothing lets the server establish that an unresolved name is a typo rather
   than a host type. An engine profile is the closest thing, and it is a fixed list. Until that
   exists the setting is the honest interface, but it is the reason the rule cannot simply be
   unconditional.
2. **`asEP_PROPERTY_ACCESSOR_MODE` under mode 3** — the setting exists
   (`angelscript.engine.propertyAccessorMode`, `--engine-property=propertyAccessorMode=<2|3>`,
   default `2`) and the undeclared-identifier rule already reads it. The rest of the accessor
   handling does not: `SemanticHelpers`' member-access fallback to `Type::get_X` still runs
   regardless of mode, so under `3` a `c.V` whose accessor lacks the `property` keyword is still
   accepted where the compiler answers `'V' is not a member of 'C'`. Missed diagnostic, not a false
   positive. `doc_r07`
3. **Numeric warnings** (`TYPE-03`) — the compiler emits three: `Implicit conversion changed sign of
   value`, `Float value truncated in implicit conversion to integer`, `Signed/Unsigned mismatch`.
   None exist here. Decidable from the source alone, so the visibility policy permits them; the
   narrowing tables at `OverloadResolver.cpp` already exist and feed only overload ranking today.
4. **Lambda body against its target funcdef** — `CheckFuncdefAssignment` only handles a named
    function on the right-hand side and returns silently for a lambda, so neither parameters nor
    return type are compared.
5. **URI normalization** — `Server::CanonicalPathFromUri` and `UriFromPath` exist and are used at no
    request entry point; handlers key their maps on the raw string, so `file:///e%3A/…` and
    `file:///E%3A/…` are different documents on Windows.
6. **`as.predefined` hot reload** — the stub is re-parsed on change but the reload does not mark the
    graph dirty, so open documents keep stale diagnostics until the next keystroke.
7. **`workspace.fileOperations`** — `didRenameFiles` / `didDeleteFiles`, plus an `#include` fixup on
    rename. `WorkspaceIncludeGraph` already holds the graph the edit needs.
8. **Exclude globs and a project root** (`PREDEF-07`) — three unbounded recursive directory walks per
    workspace root, with no way to skip build output.
9. **Untested but confirmed correct** — a corpus case for the `Foo obj(bar);` most-vexing-parse,
    where `func_declaration`'s `prec.dynamic(2)` currently outranks `variable_declaration`'s `1`.

### Out of scope

`TOOL-04` (a Debug Adapter Protocol sidecar) is a separate program, not a language-server feature.
