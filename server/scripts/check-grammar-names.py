"""No C++ file may compare against a node type, or look up a field, that the grammar does not have.

`if (nodeType == "function_definition")` compiles, runs, and is false forever. There is no warning,
no crash and no failing test - the rule it guards simply never fires. Twenty node types and four
field names in this server were exactly that: names from the C, C++ and JavaScript tree-sitter
grammars, written from memory or copied in from another project's handler, never true here.

Nineteen of the twenty were harmless - a dead alternative sitting in an `||` next to the correct
name. One was not: `document_highlight/DocumentHighlightHandler.cpp` stopped an upward walk at
`class_declaration || function_declaration`, and this grammar calls that node `func_declaration`,
so the walk stopped at classes and ran past functions. That is the shape of the bug this guard
exists for - a control that silently does nothing, with a green build on top, which is the same
shape as the two guards already in this directory.

The companion check is tests/GrammarNamesTest.cpp, and the two ask different questions. That one
asks the loaded language whether the constants in src/parser/GrammarNames.h still resolve, which is
what catches a grammar pin bump that renames a node. This one asks the source whether anyone wrote
a raw literal the grammar has never defined, which is what catches a name typed from memory.

Needs the grammar's own src/node-types.json, which CMake fetches - so unlike the other two guards
in this directory, this one runs after a configure, not before.

Run from server/:  python scripts/check-grammar-names.py
"""

import json
import re
import sys
from pathlib import Path

SERVER = Path(__file__).resolve().parent.parent
SRC = SERVER / 'src'

# Deciding whether a string is in "node type position" cannot be done by naming the variables that
# hold one - they are called nodeType, parentType, pType, pt, type, currType and half a dozen other
# things, and a list of those names would miss the next one somebody invents.
#
# So the line calibrates itself: a line that compares something against a node type the grammar DOES
# define is a line about node types, and every other string it compares is in the same position.
# `if (pt == "lambda_expression" || pt == "anonymous_function")` gives itself away, which is exactly
# the shape all twenty had - a real name and a wrong one, side by side.
NODE_TYPE_HOLDERS = re.compile(
    r'\b(?:ts_node_type\s*\(|NodeType\s*\()')
COMPARISON = re.compile(r'==\s*"([a-z][a-z_]*)"')
COMMENT = re.compile(r'^\s*(?://|\*|/\*)')

FIELD_LOOKUP = re.compile(
    r'(?:ts_node_child_by_field_name|GetChildByFieldName|GetChildByField)\s*\(\s*[^,]+,\s*"([a-z_]+)"')

# The same mistake in the other syntax. `ts_language_symbol_for_name(lang, "block", ...)` returns 0
# for a name the grammar does not have, and 0 matches nothing - so the TSSymbol comparison that was
# supposed to be faster than a string compare becomes a comparison that is never true. Same silence,
# and harder to spot, because the name is resolved far from where it is used.
SYMBOL_LOOKUP = re.compile(r'ts_language_symbol_for_name\s*\(\s*[^,]+,\s*(?:SYM_NAME\s*\(\s*)?"([a-z_]+)"')

# Words that appear in a comparison next to a node-type variable but are not node types: keyword
# text, modifier names, type names the analyzer compares by spelling.
NOT_NODE_TYPES = {
    'true', 'false', 'null', 'const', 'auto', 'void', 'this', 'super', 'shared', 'external',
    'final', 'abstract', 'override', 'explicit', 'property', 'delete', 'in', 'out', 'inout',
    'array', 'string', 'dictionary', 'get', 'set', 'value', 'name', 'public', 'private',
    'protected', 'bool', 'int', 'uint', 'float', 'double', 'mixin', 'enum', 'class', 'interface',
    'namespace', 'funcdef', 'typedef', 'import', 'from',
}


def logical_lines(text):
    """Yield (first line number, joined text) for each condition, however many lines it spans.

    A per-line reading misses the very cases this guard is for. The `||` chain that named
    `type_arguments` and `template_type` also named the real `template_type_list` - three lines
    down, which is where the calibration lives. Rejoining the condition puts them back together.
    """
    pending_number = None
    pending_parts = []

    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if pending_number is None:
            pending_number = number
        pending_parts.append(stripped)

        # A line that ends mid-expression continues into the next one.
        if stripped.endswith(('||', '&&', '(', ',', '?', ':', '==')):
            continue

        yield pending_number, ' '.join(pending_parts)
        pending_number = None
        pending_parts = []

    if pending_parts:
        yield pending_number, ' '.join(pending_parts)


def find_node_types_json():
    """The grammar checkout lives wherever the build tree is; find it rather than assume one."""
    candidates = sorted(SERVER.glob('build*/_deps/tree_sitter_angelscript-src/src/node-types.json'))
    return candidates[0] if candidates else None


def main():
    node_types_json = find_node_types_json()
    if node_types_json is None:
        print('check-grammar-names: no node-types.json found under server/build*/', file=sys.stderr)
        print('  Configure CMake first - this guard reads the grammar CMake fetches.', file=sys.stderr)
        return 0  # Not a failure: there is nothing to check against yet.

    grammar = json.loads(node_types_json.read_text(encoding='utf-8'))

    # Two different sets, and the difference matters. Every entry is a legitimate thing to compare
    # a node type against - `ts_node_type(x) == ";"` is how you spot an empty statement, and ";" is
    # an anonymous token. But only the NAMED ones may calibrate a line, because the anonymous set
    # is full of ordinary words - "return", "case", "is", "on" - and a line comparing keyword TEXT
    # would otherwise look like a line comparing node types.
    grammar_nodes = {entry['type'] for entry in grammar}
    grammar_named = {entry['type'] for entry in grammar if entry.get('named')}
    grammar_fields = set()
    for entry in grammar:
        grammar_fields.update(entry.get('fields') or {})

    bad_nodes = []
    bad_fields = []

    for path in sorted(SRC.rglob('*')):
        if path.suffix not in ('.cpp', '.h'):
            continue

        text = path.read_text(encoding='utf-8', errors='replace')

        # A file that never touches a node cannot be comparing node types. This is not a shortcut:
        # FormattingHandler.cpp compares `prev.text == "return"` against its own hand-lexed tokens
        # and has no parse tree at all, and every word it matches would read as a node type here.
        if 'ts_node' not in text:
            continue

        rel = path.relative_to(SERVER).as_posix()
        for number, line in logical_lines(text):
            if COMMENT.match(line):
                continue

            for match in FIELD_LOOKUP.finditer(line):
                field = match.group(1)
                if field not in grammar_fields:
                    bad_fields.append((rel, number, field))

            for match in SYMBOL_LOOKUP.finditer(line):
                symbol = match.group(1)
                if symbol not in grammar_nodes:
                    bad_nodes.append((rel, number, symbol))

            if 'child_by_field' in line:
                continue

            compared = [match.group(1) for match in COMPARISON.finditer(line)]
            if not compared:
                continue

            # Either the line reads a node type outright, or one of the names it compares is a real
            # node type - both mean the rest of the line is talking about node types too.
            about_node_types = (NODE_TYPE_HOLDERS.search(line) is not None
                                or any(name in grammar_named for name in compared))
            if not about_node_types:
                continue

            for candidate in compared:
                if candidate in grammar_nodes or candidate in NOT_NODE_TYPES:
                    continue
                bad_nodes.append((rel, number, candidate))

    for rel, number, name in bad_nodes:
        print('%s:%d: node type "%s" is not in the grammar - this comparison is never true' %
              (rel, number, name))
    for rel, number, name in bad_fields:
        print('%s:%d: field "%s" is not in the grammar - this lookup always returns null' %
              (rel, number, name))

    total = len(bad_nodes) + len(bad_fields)
    if total:
        print()
        print('%d name(s) the grammar does not define. Either the name is wrong, or the grammar '
              'needs it - check %s.' % (total, node_types_json.relative_to(SERVER).as_posix()))
        return 1

    print('Every node type and field named in src/ exists in the grammar.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
