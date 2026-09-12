"""Enforces architectural layer boundaries and encapsulation across the codebase.

The Layer Matrix defined in .agents/AGENTS.md:
  - Layer 1: Core / Config (config/, document/, parser/, utils/)
      Only standard C++ libraries or headers from its own layer.
      FORBIDDEN: Layer 2 (analysis), Layer 3 (features), Layer 4 (lsp).
  - Layer 2: Analysis (analysis/)
      Layer 1 and C++ libraries.
      FORBIDDEN: Layer 3 (features), Layer 4 (lsp).
  - Layer 3: Features (features/)
      Layer 1 and Layer 2.
      FORBIDDEN: Other Features (e.g. Hover must not include Completion) and Layer 4 (lsp).
  - Layer 4: Server / Listener (lsp/, main.cpp)
      Layer 1, Layer 2, and Layer 3.
      Decoupled server components (DocumentStore, AnalysisScheduler,
      PredefinedStubManager, ModuleIndex) must not include Server.h.

Additionally:
  - analysis/ and parser/ must compile without the LSP protocol library (<lsp/...>
    or utils/LspLogger.h).

Run from server/:  python scripts/check-layer-includes.py
"""

import re
import sys
from pathlib import Path

SERVER = Path(__file__).resolve().parent.parent
SRC = SERVER / 'src'

# Protocol isolation rules
GUARDED_DIRS = ('analysis', 'parser')
FORBIDDEN_ANGLED = re.compile(r'#\s*include\s*<\s*lsp/')
FORBIDDEN_QUOTED = {'utils/LspLogger.h'}

# Layer classification
LAYER_MAP = {
    'config': 1,
    'document': 1,
    'parser': 1,
    'utils': 1,
    'analysis': 2,
    'features': 3,
    'lsp': 4,
}

DECOUPLED_LSP_MODULES = {
    'lsp/DocumentStore.h',
    'lsp/AnalysisScheduler.h',
    'lsp/PredefinedStubManager.h',
    'lsp/ModuleIndex.h',
}

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


def get_layer_info(header: Path) -> tuple[int | None, str, str]:
    """Returns (layer_number, top_dir, relative_path_from_src)."""
    rel = header.resolve().relative_to(SRC.resolve()).as_posix()
    top = rel.split('/')[0]
    return LAYER_MAP.get(top, None), top, rel


def check_layer_matrix() -> list[str]:
    """Validates layer hierarchy and intra-feature encapsulation rules."""
    violations = []
    for header in sorted(SRC.rglob('*.h')):
        l_from, top_from, rel_from = get_layer_info(header)
        if l_from is None:
            continue

        text = header.read_text(encoding='utf-8', errors='replace')
        for inc in QUOTED_INCLUDE.findall(text):
            target = resolve(inc, header)
            if target is None:
                continue
            l_to, top_to, rel_to = get_layer_info(target)
            if l_to is None:
                continue

            # Check layer hierarchy (lower layer cannot include higher layer)
            if l_from < l_to:
                violations.append(
                    f"Layer violation: {rel_from} (Layer {l_from}) includes {rel_to} (Layer {l_to})"
                )
            elif l_from == 3 and l_to == 3:
                # Check cross-feature isolation in features/
                parts_from = rel_from.split('/')
                parts_to = rel_to.split('/')
                feat_from = parts_from[1] if len(parts_from) > 1 else ''
                feat_to = parts_to[1] if len(parts_to) > 1 else ''
                if feat_from and feat_to and feat_from != feat_to:
                    violations.append(
                        f"Cross-feature violation: {rel_from} (feature '{feat_from}') includes {rel_to} (feature '{feat_to}')"
                    )
            elif top_from == 'lsp' and rel_from in DECOUPLED_LSP_MODULES:
                if rel_to == 'lsp/Server.h':
                    violations.append(
                        f"Encapsulation violation: decoupled module {rel_from} includes {rel_to}"
                    )

    return violations


def main() -> int:
    problems = []

    # 1. Protocol isolation check for analysis/ and parser/
    for layer in GUARDED_DIRS:
        for header in sorted((SRC / layer).rglob('*.h')):
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

    # 2. Layer Matrix and modular encapsulation check
    matrix_violations = check_layer_matrix()
    if matrix_violations:
        print('Layer matrix or modular encapsulation violations:')
        for violation in matrix_violations:
            print(f'  - {violation}')
        return 1

    guarded = sum(len(list((SRC / layer).rglob('*.h'))) for layer in GUARDED_DIRS)
    total_headers = len(list(SRC.rglob('*.h')))
    print(f'{guarded} headers in analysis/ and parser/ compile without the LSP protocol library.')
    print(f'{total_headers} total headers conform to the Layer Matrix and modular encapsulation rules.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
