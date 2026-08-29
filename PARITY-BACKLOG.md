# Parity backlog

An issue list from a second AngelScript language server was distilled into ~60 items covering the
parser, the type system, scope, properties, lambdas, `as.predefined`, completion and the protocol.
This file records what happened when each claim was put to a real compiler, and what is left to
build.

The suite here passes at 1101 with zero unexplained false positives across six corpora, so nothing
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
| `doc_g*` | The compiler accepts it and we do **not** yet. Listed in `KnownGaps()` in `ParityAuditTest.cpp` — the entry comes out when the gap closes. |

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
| `PROP-07` abstract classes | need not implement their interface | `Missing implementation of 'void I::P()'` | Inverted: `rules/ClassRules.cpp` skips this check for abstract classes, which is a **missed** diagnostic. See WIP. `doc_r08` |
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

Both were false positives — legal code reported as an error, which is the one failure mode this
project treats as unacceptable.

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

  **Not yet shipped.** `server/cmake/TreeSitter.cmake` still pins `017b0d3`; the branch has to be
  pushed before the pin can move. Until then the fix is reachable only with
  `-DANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE=<checkout>`, and the two `KnownGaps()` entries in
  `ParityAuditTest.cpp` stay. Delete them with the pin bump.
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

---

## WIP

Real, oracle-confirmed, and none of it reports legal code today. Ordered by what a user notices
first. Each lands with its `doc_`-prefixed parity case.

1. **Initializer-list element types** — `array<int> a = {"x"}` is silent here and is not silent for
   real: `Can't implicitly convert from 'const string' to 'int&'`. `InitializerListChecker`
   validates shape against the list pattern but never an element's type. It also only visits
   `variable_declaration` initializers — not call arguments, assignments or returns — and never
   checks element *count*. Note for whoever does the count: an omitted element produces no node, so
   `{ 0, 1, , 4, 5 }` arrives as four elements, not five. `doc_r10`
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
4. **Completion hygiene** — suppress completion inside comments and string literals, and after the
   `:` of a `case X:` label. `CompletionHandler::GetCompletion` is handed `request.tree` and never
   looks at it, so `case FOO:` currently offers every local, every global and all 60 keywords.
5. **Typedef unwrapping inside template arguments** — `OverloadResolver` unwraps a typedef only at
   the outer name, so a host declaring `typedef uint8 byte;` still fails on `array<byte>` against
   `array<uint8>`. This is the true, narrow form of `TYPE-05`.
6. **Abstract classes must still implement their interfaces** — `rules/ClassRules.cpp` skips the
   check when the class is abstract. Only emit where the whole hierarchy is visible. `doc_r08`
7. **Bracket-array member resolution** — `SemanticHelpers::CleanBaseType` reduces `int[]` to `int`,
    so `arr.length()` on a bracket-declared variable gets no hover, no completion and no checking
    (`CallChecker` bails at the visibility guard). Normalization lives in exactly two places today
    and one of them hardcodes `"array"` rather than reading `config.types.arrayTypeName`.
8. **Lambda body against its target funcdef** — `CheckFuncdefAssignment` only handles a named
    function on the right-hand side and returns silently for a lambda, so neither parameters nor
    return type are compared.
9. **Client failure surface** — `extension.ts` swallows a start failure into an output channel with
    no `showErrorMessage`, no `errorHandler` and no status bar. Highest value per line on the list.
10. **`angelscript.restartServer`** — needs `workspace/executeCommand` server-side and
    `contributes.commands` client-side.
11. **Formatter** — every `{` goes onto its own line unconditionally, so `array<int> a = {1,2,3};`
    and every lambda argument are exploded Allman-style; `#if` is forced to column 0; a metadata
    block is joined onto the declaration line.
12. **Hover doc comments** — the handler reads the *hovered* file's text at the *declaring* symbol's
    line, so a cross-file hover can show an unrelated comment from the local file. The correct
    pattern is already in `ResolveCompletionItem`. Then inherit documentation from an overridden
    interface method (`SUGG-04`).
13. **URI normalization** — `Server::CanonicalPathFromUri` and `UriFromPath` exist and are used at no
    request entry point; handlers key their maps on the raw string, so `file:///e%3A/…` and
    `file:///E%3A/…` are different documents on Windows.
14. **`as.predefined` hot reload** — the stub is re-parsed on change but the reload does not mark the
    graph dirty, so open documents keep stale diagnostics until the next keystroke.
15. **`workspace.fileOperations`** — `didRenameFiles` / `didDeleteFiles`, plus an `#include` fixup on
    rename. `WorkspaceIncludeGraph` already holds the graph the edit needs.
16. **Exclude globs and a project root** (`PREDEF-07`) — three unbounded recursive directory walks per
    workspace root, with no way to skip build output.
17. **Untested but confirmed correct** — a corpus case for the `Foo obj(bar);` most-vexing-parse,
    where `func_declaration`'s `prec.dynamic(2)` currently outranks `variable_declaration`'s `1`.

### Out of scope

`TOOL-04` (a Debug Adapter Protocol sidecar) is a separate program, not a language-server feature.
