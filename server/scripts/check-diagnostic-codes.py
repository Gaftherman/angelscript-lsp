"""Every diagnostic message must belong to a rule that can actually fire.

A message translated into two languages for a rule that never runs is worse than no message: it
reads as a feature to anyone browsing the table, and someone eventually implements it a second time.
The reverse drift is worse still - a rule that quietly stops being emitted keeps its message, so
nothing looks wrong.

Six codes are deliberately never emitted. Each names a real AngelScript rule that this analyzer
cannot decide yet, and the argument for each sits in i18n.cpp's header and at the rule's own site.
This script pins that set: a seventh dead code fails, and so does one of the six coming back to life
without the documentation being updated.

Run from server/:  python scripts/check-diagnostic-codes.py
"""

import re
import sys
from pathlib import Path

SERVER = Path(__file__).resolve().parent.parent
I18N = SERVER / 'src' / 'i18n' / 'i18n.cpp'
CODES_HEADER = SERVER / 'src' / 'analysis' / 'DiagnosticCodes.h'

# Kept in step with the block at the top of i18n.cpp, which carries the reasoning.
DELIBERATELY_NEVER_EMITTED = {
    'as-err-base-not-found',
    'as-err-invalid-reference-return',
    'as-err-no-matching-operator',
    'as-err-readonly-handle',
    'as-err-standalone-reference',
    'as-warn-shadow-global',
}

CODE_PATTERN = re.compile(r'"(as-(?:err|warn|syntax|hint)-[a-z0-9-]+)"')


def declared_codes() -> set:
    """Codes the message table defines, which is what the user can be shown."""
    text = I18N.read_text(encoding='utf-8')
    return {m.group(1) for m in re.finditer(r'm_messages\["(as-[a-z0-9-]+)"\]', text)}


def referenced_codes() -> set:
    """Codes named anywhere a diagnostic is produced.

    Both spellings count: the literal at an emit site, and the constant in DiagnosticCodes.h, which
    several rules use instead. A code reachable either way is live.
    """
    referenced = set()

    constant_of = {}
    if CODES_HEADER.exists():
        header = CODES_HEADER.read_text(encoding='utf-8')
        for match in re.finditer(r'(\w+)\s*=\s*"(as-[a-z0-9-]+)"', header):
            constant_of[match.group(1)] = match.group(2)

    for path in list((SERVER / 'src').rglob('*.cpp')) + list((SERVER / 'src').rglob('*.h')):
        if path == I18N or path == CODES_HEADER:
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        referenced.update(m.group(1) for m in CODE_PATTERN.finditer(text))
        for name, code in constant_of.items():
            # `codes::NotAllPathsReturn` - the constant is how the emit site names it.
            if re.search(r'\b' + re.escape(name) + r'\b', text):
                referenced.add(code)

    return referenced


def severity_mismatches() -> list:
    """Emit sites where an `as-err-` code is produced at a severity that is not Error.

    The prefix is the promise the user reads: `as-err-` in the Problems panel next to a yellow
    squiggle is the code contradicting itself. A rule that has to hedge is a warning and should say
    so in its name - which is what happened to as-warn-undeclared-identifier.

    A statement naming both severities is not a mismatch: `named-argument-syntax` is an Error unless
    the engine's own alterSyntaxNamedArgs mode says the compiler only warns, and that conditional is
    the rule being right rather than sloppy.
    """
    err_code = re.compile(r'"(as-err-[a-z0-9-]+)"')
    severity = re.compile(r'DiagnosticSeverity::(\w+)')

    found = []
    for path in list((SERVER / 'src').rglob('*.cpp')) + list((SERVER / 'src').rglob('*.h')):
        if path in (I18N, CODES_HEADER):
            continue
        for statement in path.read_text(encoding='utf-8', errors='replace').split(';'):
            codes = err_code.findall(statement)
            severities = set(severity.findall(statement))
            if not codes or not severities or 'Error' in severities:
                continue
            for code in codes:
                found.append((code, path.name, sorted(severities)[0]))

    return found


def main() -> int:
    declared = declared_codes()
    referenced = referenced_codes()

    if not declared:
        print('Found no diagnostic codes in i18n.cpp - has the message table moved?', file=sys.stderr)
        return 2

    dead = declared - referenced
    unexpected_dead = sorted(dead - DELIBERATELY_NEVER_EMITTED)
    resurrected = sorted(DELIBERATELY_NEVER_EMITTED - dead)
    undeclared = sorted(referenced - declared)

    problems = False

    if unexpected_dead:
        problems = True
        print('These codes have a message but are emitted from nowhere:', file=sys.stderr)
        for code in unexpected_dead:
            print(f'  - {code}', file=sys.stderr)
        print('  Either emit them, or delete the code and its translations. If the rule is real but'
              '\n  undecidable for now, add it to DELIBERATELY_NEVER_EMITTED here and write the'
              '\n  reason into i18n.cpp\'s header block and the rule\'s own site.', file=sys.stderr)

    if resurrected:
        problems = True
        print('\nThese are listed as never emitted, but something emits them now:', file=sys.stderr)
        for code in resurrected:
            print(f'  - {code}', file=sys.stderr)
        print('  Remove them from DELIBERATELY_NEVER_EMITTED and from i18n.cpp\'s header block.',
              file=sys.stderr)

    mismatched = severity_mismatches()
    if mismatched:
        problems = True
        print('\nThese are named as errors but are emitted at another severity:', file=sys.stderr)
        for code, where, sev in mismatched:
            print(f'  - {code} in {where} at {sev}', file=sys.stderr)
        print('  A rule that has to hedge belongs under an as-warn- name. A rule that is certain'
              '\n  should say Error. Renaming means the i18n table and every test naming the code.',
              file=sys.stderr)

    if undeclared:
        problems = True
        print('\nThese are emitted but have no message, so the user would see the raw code:',
              file=sys.stderr)
        for code in undeclared:
            print(f'  - {code}', file=sys.stderr)

    if problems:
        return 1

    print(f'{len(declared)} diagnostic codes declared, '
          f'{len(declared) - len(dead)} live, '
          f'{len(dead)} documented as never emitted.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
