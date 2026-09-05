"""The grammar this build compiles must be the grammar cmake/TreeSitter.cmake names.

The grammar is pinned by commit, not by branch or tag, and it gets bumped: the pin has moved three
times, each move recorded in TreeSitter.cmake's own comments. Between the pin moving and the build
tree being refreshed there is a window where the code is compiled against a grammar nobody chose,
and nothing says so - the build succeeds, the tests pass or fail for reasons that look unrelated,
and the parity audit structurally cannot see it, because an unparseable construct costs a symbol
from the index rather than producing a diagnostic. PARITY-BACKLOG.md records that limitation.

This is the second half of the pair that starts with tests/GrammarNamesTest.cpp. That one asks the
LOADED language whether the names the code uses still resolve, which catches a bump that renames a
node. This one asks whether the checkout is the commit that was asked for at all - which catches a
stale build tree, a hand-edited checkout, and a pin bumped without a reconfigure.

A local checkout via ANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE is the documented way to work on the
grammar (README.md), and by definition is not at the pin. That case reports and passes.

Run from server/:  python scripts/check-grammar-pin.py
"""

import re
import subprocess
import sys
from pathlib import Path

SERVER = Path(__file__).resolve().parent.parent
CMAKE = SERVER / 'cmake' / 'TreeSitter.cmake'


def pinned_commit():
    """The commit TreeSitter.cmake asks FetchContent for."""
    text = CMAKE.read_text(encoding='utf-8')
    match = re.search(
        r'FetchContent_Declare\(\s*tree_sitter_angelscript\b[^)]*?GIT_TAG\s+([0-9a-fA-F]{7,40})',
        text, re.DOTALL)
    return match.group(1).lower() if match else None


def checkouts():
    """Every fetched grammar checkout under a build tree - there may be more than one."""
    return sorted(SERVER.glob('build*/_deps/tree_sitter_angelscript-src'))


def head_of(path):
    try:
        result = subprocess.run(['git', '-C', str(path), 'rev-parse', 'HEAD'],
                                capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip().lower() if result.returncode == 0 else None


def main():
    pinned = pinned_commit()
    if pinned is None:
        print('check-grammar-pin: no GIT_TAG found for tree_sitter_angelscript in '
              + CMAKE.relative_to(SERVER).as_posix(), file=sys.stderr)
        return 2

    found = checkouts()
    if not found:
        print('check-grammar-pin: no grammar checkout under server/build*/ - configure CMake first.')
        return 0

    problems = []
    for path in found:
        where = path.relative_to(SERVER).as_posix()
        head = head_of(path)

        if head is None:
            # A checkout with no git metadata cannot be compared. That is what a local grammar
            # checkout copied into place looks like, and it is not a failure.
            print(f'  {where}: no git metadata, not compared')
            continue

        if head.startswith(pinned) or pinned.startswith(head):
            print(f'  {where}: at the pinned commit {pinned[:12]}')
        else:
            problems.append((where, head))

    if problems:
        print(f'\nThe pin in {CMAKE.relative_to(SERVER).as_posix()} names {pinned[:12]}, but:',
              file=sys.stderr)
        for where, head in problems:
            print(f'  - {where} is at {head[:12]}', file=sys.stderr)
        print('  The build is compiling a grammar nobody chose. Either reconfigure so FetchContent'
              '\n  moves the checkout to the pin, or - if the checkout is what you want - bump the'
              '\n  pin and say in the commit what the new commit changes, the way the three'
              '\n  previous bumps are recorded in that file.', file=sys.stderr)
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
