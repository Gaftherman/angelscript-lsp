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

## Adding a case

Ask the compiler first, then write the test to its answer:

```bash
build/Release/angelscript_oracle yourcase.as --json
```

Note that `grid` is a registered type here (the SDK's `scriptgrid` add-on), so it cannot be used as
a variable name — AS-Harness does not register it, which is the kind of surface difference that
makes pairing matter.
