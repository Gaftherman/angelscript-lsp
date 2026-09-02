"""The analysis and parser layers must compile without the LSP protocol library.

`analysis/SymbolTable.h` used to include `utils/LspLogger.h`, which includes
`<lsp/messagehandler.h>` and `<lsp/messages.h>`. Every translation unit in `analysis/` therefore
compiled the whole protocol library, and `SymbolTable` could not be tested without it - a header
nobody would think to look at decided the dependencies of a layer that has no business knowing the
protocol exists.

It was one `#include` to reintroduce and nothing would have said so: the code still compiles, the
tests still pass, and the coupling is invisible until someone reads the transitive include tree by
hand. That is the same shape as the two guards already in this directory - a control that silently
stops working, with a green build on top.

The rule: no header under `src/analysis/` or `src/parser/` may reach `<lsp/...>` or
`utils/LspLogger.h`, directly or through another project header. A `.cpp` in those layers may -
it is the translation unit's own business - and `src/lsp/` may, obviously.

Run from server/:  python scripts/check-layer-includes.py
"""

import re
import sys
from pathlib import Path

SERVER = Path(__file__).resolve().parent.parent
SRC = SERVER / 'src'

# Layers that must stay free of the protocol, and what counts as reaching it.
GUARDED_DIRS = ('analysis', 'parser')
FORBIDDEN_ANGLED = re.compile(r'#\s*include\s*<\s*lsp/')
FORBIDDEN_QUOTED = {'utils/LspLogger.h'}

QUOTED_INCLUDE = re.compile(r'#\s*include\s*"([^"]+)"')


def resolve(include: str, origin: Path) -> Path | None:
    """A quoted include is written either from src/ or beside the including file."""
    for candidate in (SRC / include, origin.parent / include):
        if candidate.is_file():
            return candidate.resolve()
    return None


def reaches_protocol(header: Path, seen: set[Path]) -> list[str] | None:
    """Returns the include chain that reaches the protocol, or None when it does not."""
    if header in seen:
        return None
    seen.add(header)

    text = header.read_text(encoding='utf-8', errors='replace')

    if FORBIDDEN_ANGLED.search(text):
        return [header.name]

    for include in QUOTED_INCLUDE.findall(text):
        if include in FORBIDDEN_QUOTED:
            return [header.name, include]

        target = resolve(include, header)
        if target is None or target.suffix != '.h':
            continue

        chain = reaches_protocol(target, seen)
        if chain is not None:
            return [header.name] + chain

    return None


def main() -> int:
    problems = []

    for layer in GUARDED_DIRS:
        for header in sorted((SRC / layer).rglob('*.h')):
            # A fresh `seen` per header: a shared one would let the first header consume a shared
            # dependency and hide the same violation in every header after it.
            chain = reaches_protocol(header.resolve(), set())
            if chain is not None:
                problems.append((header.relative_to(SERVER), ' -> '.join(chain)))

    if problems:
        print('Headers in analysis/ or parser/ that pull in the LSP protocol library:')
        for path, chain in problems:
            print(f'  - {path}\n      {chain}')
        print()
        print('Hold the logger by a forward declaration and move the include into the .cpp:')
        print('    namespace angel_lsp::utils { class LspLogger; }')
        print()
        print('These layers are meant to be usable, and testable, without the protocol.')
        return 1

    guarded = sum(len(list((SRC / layer).rglob('*.h'))) for layer in GUARDED_DIRS)
    print(f'{guarded} headers in analysis/ and parser/ compile without the LSP protocol library.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
