"""Generate GrammarNames.h from the grammar's own node-types.json."""
import json, io, re

nt = json.load(io.open('build/_deps/tree_sitter_angelscript-src/src/node-types.json', encoding='utf-8'))

nodes = sorted({n['type'] for n in nt if n.get('named')})
fields = set()
for n in nt:
    for f in (n.get('fields') or {}):
        fields.add(f)
fields = sorted(fields)


def pascal(name):
    return ''.join(part.capitalize() for part in name.split('_'))


out = []
out.append('''#pragma once

#include <cstdint>
#include <string_view>

#include <tree_sitter/api.h>

/**
 * @file
 * @brief Every node type and field name the grammar defines, as constants.
 *
 * Generated from the grammar's own `src/node-types.json` - the names here are copied from
 * tree-sitter-angelscript, not typed out. That is the whole point.
 *
 * Before this header there was nothing checking that a name written in C++ existed in the grammar,
 * and twenty did not. `if (nodeType == "function_definition")` compiles, runs, and is false
 * forever; `ts_node_child_by_field_name(node, "initializer", 11)` compiles, runs, and returns null
 * forever. Most were names from the C, C++ and JavaScript grammars, copied in from another
 * project's handler and never true here. One - `function_declaration`, where this grammar says
 * `func_declaration` - was a live bug that stopped a search at classes and not at functions.
 *
 * Two guards keep it that way, and they answer different questions:
 *
 *   - tests/GrammarNamesTest.cpp asks the LOADED language whether each constant below still
 *     resolves. That is what turns a grammar pin bump into a failing test with a name in it,
 *     instead of a rule that quietly stops matching. The parity audit structurally cannot catch
 *     this: an unparseable construct costs a symbol rather than producing a diagnostic, so a
 *     grammar gap reaches it as silence. See PARITY-BACKLOG.md.
 *
 *   - scripts/check-grammar-names.py asks the SOURCE whether anyone wrote a raw string literal in
 *     a node-type or field position that the grammar does not define. That is what catches a new
 *     name typed from memory, which is how all twenty arrived.
 *
 * Regenerate with scripts/gen-grammar-names.py after bumping the grammar pin in
 * cmake/TreeSitter.cmake.
 */
namespace angel_lsp::parser
{
    /** @brief Named node types. Anonymous tokens - "class", "int", "(" - are deliberately absent. */
    namespace nodes
    {''')

for n in nodes:
    out.append('        inline constexpr std::string_view %-28s = "%s";' % (pascal(n), n))

out.append('''    }

    /** @brief Field names, for ts_node_child_by_field_name and the helper below. */
    namespace fields
    {''')

for f in fields:
    out.append('        inline constexpr std::string_view %-20s = "%s";' % (pascal(f), f))

out.append('''    }

    /**
     * @brief ts_node_child_by_field_name with the length taken from the name itself.
     *
     * The reason this exists is that 221 of the 344 field lookups in this server passed the length
     * as a separate integer literal - `"consequence", 11` - and 31 more declared a per-file
     * `k_consequenceFieldLength` constant to say the same thing. All of them happened to be
     * correct; none of them had to be.
     */
    [[nodiscard]] inline TSNode GetChildByField(TSNode parent, std::string_view fieldName) noexcept
    {
        if (ts_node_is_null(parent))
        {
            return TSNode{};
        }
        return ts_node_child_by_field_name(parent, fieldName.data(),
                                           static_cast<uint32_t>(fieldName.length()));
    }

    /** @brief Every constant in nodes::, for the test that checks they still resolve. */
    inline constexpr std::string_view k_allNodeTypes[] = {''')

for n in nodes:
    out.append('        nodes::%s,' % pascal(n))

out.append('''    };

    /** @brief Every constant in fields::, same purpose. */
    inline constexpr std::string_view k_allFieldNames[] = {''')

for f in fields:
    out.append('        fields::%s,' % pascal(f))

out.append('''    };
}
''')

io.open('src/parser/GrammarNames.h', 'w', encoding='utf-8', newline='').write('\n'.join(out))
print('generated src/parser/GrammarNames.h: %d node types, %d fields' % (len(nodes), len(fields)))
