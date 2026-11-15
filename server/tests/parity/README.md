# Parity corpus

Scripts whose verdict a real AngelScript compiler has already given, used to check that this
analyzer agrees with it. Every one of them was written while chasing a specific question, and the
compiler's answer — not an assumption about the language — is what the test asserts.

Run them with the oracle built from the SDK:

```bash
cmake -B build -S . -DANGELLSP_BUILD_ORACLE=ON
cmake --build build --config Release --target angelscript_oracle angel_lsp_tests

ASHARNESS_EXE=build/Release/angelscript_oracle \
PARITY_PREDEFINED=tests/fixtures/sdk-addons.as.predefined \
PARITY_SCRIPT_DIR=tests/parity \
build/Release/angel_lsp_tests --no-skip --test-case="*Parity*"
```

`UNEXPLAINED FALSE POSITIVES` must be **0**. That is the direction that matters: on a script the
real compiler accepts, this analyzer must report nothing. The reverse — the compiler rejects and we
stay silent — is counted and printed but not failed on, because this is not a compiler and does not
claim to be one.

## Pairing

A corpus is only meaningful next to the stub it was written against. `PARITY_PREDEFINED` is not
optional here: `sdk-addons.as.predefined` describes exactly what `tools/oracle/main.cpp` registers,
and running against a different stub produces findings about the mismatch instead of about the
analyzer. Running the whole 1000-file `angelscript/` corpus, for instance, tells you nothing — every
file there names host types (`edict_t`, `CBaseEntity`) that live in a game engine, and the compiler
rejects all of them.

## Prefixes

| Prefix | What it pins down |
|---|---|
| `qa_` | Constructors as methods, sized array construction, nested templates, bracket arrays |
| `il_` | Initializer lists: which nestings the list factory accepts |
| `tpl_` | `optional<T>` and `dictionary` — that a template's list pattern cannot be guessed |
| `fe_` | `foreach`: element typing, two-variable form, invalid containers |
| `tc_` | `try` / `catch` and which paths count as returning |
| `doc_p` | Claims from the external issue list that the compiler **accepts** and we do too |
| `doc_r` | Claims the compiler **rejects** - several of them refute the list outright |
| `doc_g` | Claims the compiler accepts and we do not yet; each has an entry in `KnownGaps()` |

The `doc_` set came from a second AngelScript language server's issue tracker, distilled into ~60
claims. Nine of them turned out to be wrong about the language, and four described behaviour this
analyzer already had correct. `PARITY-BACKLOG.md` at the repository root carries the verdict for
each, with the compiler output that settled it - read it before implementing anything from that
list.

## Adding a case

Ask the compiler first, then write the test to its answer:

```bash
build/Release/angelscript_oracle yourcase.as --json
```

Note that `grid` is a registered type here (the SDK's `scriptgrid` add-on), so it cannot be used as
a variable name — AS-Harness does not register it, which is the kind of surface difference that
makes pairing matter.

## Differential corpus (`differential/`)

Each category is a directory of `.as` files checked against the real compiler by
`scripts/differential-validate.py` and replayed against the server by `DifferentialCorpusTest`.

- **Multi-case files.** One file holds many independent cases, each introduced by
  `// ==== case: <id> ====`. Every section is compiled and analysed on its own, so names never collide
  between sections and ids must be unique inside a file. A case that needs sibling files (`#include`)
  stays a directory with a `main.as`.
- **Header tags** (inside a section): `// @expect: accept|reject`, `// @complete+ Label`,
  `// @complete- Label`, `// @scan: all`, with `/*$complete*/` marking the cursor.
- **Ranges.** `// @error: L:C-L:C` pins the exact 0-based range of a diagnostic, with lines counted from
  the first line after the marker. `// @range: strict` requires a server range to cover the position the
  compiler reports (or sit where a missing token belongs); `// @range: loose` allows one line of
  distance or only blanks and comments in between.
- **Known gaps.** `_known_gaps/` holds cases the compiler rejects and the server does not (or reports at
  another place). The test skips every directory starting with `_`; the validator still lists them.
