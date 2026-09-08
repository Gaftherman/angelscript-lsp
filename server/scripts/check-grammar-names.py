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

Two grammars now, since the doc-comment renderer parses Doxygen. A name real in one is imaginary in
the other, so each file is checked against the grammar it actually parses - `brief_header` is a
doxygen type and `func_declaration` an AngelScript one, and each is an error in the other's file.
Checking everything against one grammar was not a smaller version of this: it let
`src/analysis/DoxygenMarkdown.cpp` through entirely, because the calibration only fires on a line
that names a type the grammar DOES define, and that file names none of AngelScript's.

That file also spells the comparison `std::strcmp(type, "brief_header") == 0`, with the literal to
the left of the operator, which the original pattern never saw. Both spellings are read now, and the
strcmp form calibrates on the variable rather than the line - `type` is whatever the file last
assigned from `ts_node_type`.

Needs the grammar's own src/node-types.json, which CMake fetches - so unlike the other two guards
in this directory, this one runs after a configure, not before. It also reads grammar.json beside
it, for the hidden rules: `_text_line` is absent from node-types.json and is still what
`ts_node_type` returns for a plain body line, and reporting it would be this guard crying wolf.

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

# `ts_node_type` returns a `const char *`, so the other natural spelling of the same comparison puts
# the literal to the LEFT of the operator and the regex above never sees it:
#
#     if (std::strcmp(type, "brief_header") == 0)
#
# DoxygenMarkdown.cpp is written entirely in this form. Without this pattern the guard read that
# file, found no comparison it recognised, and passed - enforcing nothing on the one file in the
# server most likely to name a node that does not exist, which is how it was caught.
STRCMP_COMPARISON = re.compile(r'\bstrcmp\s*\(\s*([A-Za-z_]\w*)\s*,\s*"([a-z_][a-z_]*)"\s*\)')

# The per-line calibration cannot fire on the strcmp form: `strcmp(type, "_text_line") == 0` names
# no node type the grammar defines, so nothing on that line gives it away as being about node types.
# What gives it away is one line earlier - `const char *type = ts_node_type(child);` - so the
# variable is what carries the calibration here, and it is collected per file.
NODE_TYPE_VARIABLE = re.compile(r'\b([A-Za-z_]\w*)\s*=\s*ts_node_type\s*\(')
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


# This server parses two languages, and a name that is real in one is imaginary in the other.
# `brief_header` and `storageclass` are doxygen node types and are not in the AngelScript grammar;
# `func_declaration` is the reverse. Checking every file against one grammar would either report the
# doxygen renderer as full of errors, or - the way this guard was written before doxygen arrived -
# let it through unchecked, because the calibration only fires on a line that names a node type the
# grammar does define, and a file that names none of them is invisible.
#
# So each file is checked against the grammar it actually parses. A file that reaches for
# DoxygenParser, or names the doxygen language entry point, is talking about doxygen nodes.
GRAMMARS = {
    'angelscript': 'tree_sitter_angelscript-src',
    'doxygen': 'tree_sitter_doxygen-src',
}
DOXYGEN_MARKERS = ('parser/DoxygenParser.h', 'tree_sitter_doxygen')


def find_node_types_json(checkout):
    """The grammar checkout lives wherever the build tree is; find it rather than assume one."""
    candidates = sorted(SERVER.glob('build*/_deps/%s/src/node-types.json' % checkout))
    return candidates[0] if candidates else None


def load_grammar(path):
    """Reads one grammar's node-types.json into the three sets the checks below need.

    node-types.json is not the whole truth about what `ts_node_type` can return: it lists the
    VISIBLE types, and a rule whose name begins with an underscore is hidden from it while still
    being a real symbol at runtime. `_text_line` is exactly that - absent from the doxygen
    node-types.json, present in its parser.c as `[sym__text_line] = "_text_line"`, and returned by
    `ts_node_type` for every plain body line. Reporting it would be this guard crying wolf, so the
    hidden rule names are read from grammar.json beside it and accepted.

    They are accepted, not trusted: a hidden name may be compared against but may never calibrate a
    line, for the same reason the anonymous tokens may not.
    """
    grammar = json.loads(path.read_text(encoding='utf-8'))
    fields = set()
    for entry in grammar:
        fields.update(entry.get('fields') or {})

    hidden = set()
    grammar_json = path.parent / 'grammar.json'
    if grammar_json.exists():
        rules = json.loads(grammar_json.read_text(encoding='utf-8')).get('rules') or {}
        hidden = {name for name in rules if name.startswith('_')}

    return {
        'nodes': {entry['type'] for entry in grammar} | hidden,
        'named': {entry['type'] for entry in grammar if entry.get('named')},
        'fields': fields,
        'path': path,
    }


def grammar_for(text):
    """Which grammar's names a file is entitled to use, decided by what it parses."""
    return 'doxygen' if any(marker in text for marker in DOXYGEN_MARKERS) else 'angelscript'


def main():
    # Two different sets per grammar, and the difference matters. Every entry is a legitimate thing
    # to compare a node type against - `ts_node_type(x) == ";"` is how you spot an empty statement,
    # and ";" is an anonymous token. But only the NAMED ones may calibrate a line, because the
    # anonymous set is full of ordinary words - "return", "case", "is", "on" - and a line comparing
    # keyword TEXT would otherwise look like a line comparing node types.
    loaded = {}
    for name, checkout in GRAMMARS.items():
        found = find_node_types_json(checkout)
        if found is not None:
            loaded[name] = load_grammar(found)

    if 'angelscript' not in loaded:
        print('check-grammar-names: no node-types.json found under server/build*/', file=sys.stderr)
        print('  Configure CMake first - this guard reads the grammar CMake fetches.', file=sys.stderr)
        return 0  # Not a failure: there is nothing to check against yet.

    bad_nodes = []
    bad_fields = []
    unchecked = set()

    for path in sorted(SRC.rglob('*')):
        if path.suffix not in ('.cpp', '.h'):
            continue

        text = path.read_text(encoding='utf-8', errors='replace')

        # A file that never touches a node cannot be comparing node types. This is not a shortcut:
        # FormattingHandler.cpp compares `prev.text == "return"` against its own hand-lexed tokens
        # and has no parse tree at all, and every word it matches would read as a node type here.
        if 'ts_node' not in text:
            continue

        # A file whose grammar this checkout has not fetched cannot be checked. Skipping it is the
        # honest outcome, but say so - silence here is the exact failure this guard exists to catch.
        wanted = grammar_for(text)
        if wanted not in loaded:
            unchecked.add(wanted)
            continue

        grammar_nodes = loaded[wanted]['nodes']
        grammar_named = loaded[wanted]['named']
        grammar_fields = loaded[wanted]['fields']

        node_type_vars = set(NODE_TYPE_VARIABLE.findall(text))

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
            strcmp_hits = [(match.group(1), match.group(2))
                           for match in STRCMP_COMPARISON.finditer(line)]
            compared += [name for _, name in strcmp_hits]
            if not compared:
                continue

            # Either the line reads a node type outright, one of the names it compares is a real
            # node type, or it strcmps a variable this file filled from ts_node_type - all three
            # mean the rest of the line is talking about node types too.
            about_node_types = (NODE_TYPE_HOLDERS.search(line) is not None
                                or any(name in grammar_named for name in compared)
                                or any(var in node_type_vars for var, _ in strcmp_hits))
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
        print('%d name(s) no grammar defines. Either the name is wrong, or the grammar needs it - '
              'check %s.' % (total, ', '.join(sorted(
                  loaded[name]['path'].relative_to(SERVER).as_posix() for name in loaded))))
        return 1

    for name in sorted(unchecked):
        print('check-grammar-names: the %s grammar is not in this build tree; files that parse it '
              'went unchecked.' % name, file=sys.stderr)

    print('Every node type and field named in src/ exists in the grammar it parses (%s).'
          % ', '.join(sorted(loaded)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
