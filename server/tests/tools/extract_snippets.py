#!/usr/bin/env python3
"""Extract the AngelScript snippets embedded in this suite's C++ tests.

Why this exists
---------------
ParityAuditTest compares this analyzer against the real AngelScript compiler, but AS-Harness only
ships ten scripts to compare against. This suite's own tests contain hundreds of AngelScript
snippets as C++ raw string literals, and those are a far better oracle corpus: they are the code
this project's expectations are actually written about. Running them through the real compiler is
what says whether those expectations match the language.

It found two real defects the unit tests could not, because the tests encoded the same wrong belief
the analyzer did:

  * `as-err-unary-neg-on-unsigned` reported an error on `-someUint`. AngelScript permits it (the
    result wraps, as in C). The rule was deleted; the test that asserted it now asserts the
    opposite.
  * Two identical signatures arriving from two loaded stubs were reported as an ambiguous call.

Usage
-----
    python server/tests/tools/extract_snippets.py <output-dir>

    set ASHARNESS_EXE=...\\asharness.exe
    set PARITY_SCRIPT_DIR=<output-dir>
    Debug\\angel_lsp_tests.exe --no-skip --test-case="*Parity*"

Snippets that the real compiler rejects are not failures. Most are deliberate fragments testing one
rule and do not compile standalone; the audit only asserts on the files it *accepts*.
"""

import glob
import hashlib
import os
import re
import sys

# The two raw-string delimiters this suite uses.
PATTERNS = [
    re.compile(r'R"\((.*?)\)"', re.S),
    re.compile(r'R"AS\((.*?)\)AS"', re.S),
]

# A snippet has to look like AngelScript to be worth compiling.
ANGELSCRIPT_HINT = re.compile(
    r"\b(void|int|float|double|bool|string|class|interface|enum|funcdef|namespace|array<|dictionary)\b"
)

# ServerHarnessTest embeds LSP frames in the same raw-string form; those are not AngelScript.
JSON_HINT = re.compile(r'"jsonrpc"|"method"\s*:|"textDocument"|Content-Length')

MIN_BODY_CHARS = 20


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    tests_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(tests_dir)

    kept = 0
    skipped_json = 0
    skipped_trivial = 0
    seen = set()

    sources = sorted(glob.glob("*.cpp")) + sorted(glob.glob("helpers/*.h"))

    for path in sources:
        with open(path, encoding="utf-8", errors="replace") as handle:
            src = handle.read()

        for pattern in PATTERNS:
            for match in pattern.finditer(src):
                body = match.group(1)

                if JSON_HINT.search(body):
                    skipped_json += 1
                    continue

                if not ANGELSCRIPT_HINT.search(body) or len(body.strip()) < MIN_BODY_CHARS:
                    skipped_trivial += 1
                    continue

                # Content-hashed so the same snippet appearing in several tests is written once and
                # the filenames stay stable across runs.
                digest = hashlib.sha1(body.encode("utf-8", "replace")).hexdigest()[:10]
                if digest in seen:
                    continue
                seen.add(digest)

                stem = os.path.splitext(os.path.basename(path))[0]
                target = os.path.join(out_dir, f"{stem}_{digest}.as")
                with open(target, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(body)

                kept += 1

    print(f"extracted {kept} AngelScript snippets to {out_dir}")
    print(f"  skipped (JSON/LSP frames)   : {skipped_json}")
    print(f"  skipped (too small / not AS): {skipped_trivial}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
