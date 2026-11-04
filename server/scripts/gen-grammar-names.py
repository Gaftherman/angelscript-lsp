"""Generate GrammarNames.h from the grammar's own node-types.json."""
import json
import sys
from pathlib import Path

SERVER = Path(__file__).resolve().parent.parent
REPO_ROOT = SERVER.parent

# Locate the fetched grammar's node-types.json across server/build* or root build*
candidates = sorted(SERVER.glob('build*/_deps/tree_sitter_angelscript-src/src/node-types.json'))
if not candidates:
    candidates = sorted(REPO_ROOT.glob('build*/_deps/tree_sitter_angelscript-src/src/node-types.json'))

if not candidates:
    print('gen-grammar-names: no node-types.json found under server/build*/ or build*/', file=sys.stderr)
    print('  Configure CMake first so FetchContent downloads the grammar.', file=sys.stderr)
    sys.exit(1)

node_types_path = candidates[0]
nt = json.loads(node_types_path.read_text(encoding='utf-8'))

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

#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

/**
 * @file
 * @brief Every node type and field name the grammar defines, as constants.
 *        Generated from the grammar's own `src/node-types.json` - the names here are copied from
 */
namespace angel_lsp::parser
{
/**
 * @brief Named node types. Anonymous tokens - "class", "int", "(" - are deliberately absent.
 */
namespace nodes
{''')

for n in nodes:
    out.append('inline constexpr std::string_view %s = "%s";' % (pascal(n), n))

out.append('''\
} // namespace nodes

/**
 * @brief Field names, for ts_node_child_by_field_name and the helper below.
 */
namespace fields
{''')

for f in fields:
    out.append('inline constexpr std::string_view %s = "%s";' % (pascal(f), f))

out.append('''\
} // namespace fields

/**
 * @brief ts_node_child_by_field_name with the length taken from the name itself.
 *        The reason this exists is that 221 of the 344 field lookups in this server passed the length
 */
[[nodiscard]] inline TSNode GetChildByField(TSNode parent, std::string_view fieldName) noexcept
{
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }
    return ts_node_child_by_field_name(parent, fieldName.data(), static_cast<uint32_t>(fieldName.length()));
}

/**
 * @brief Retrieves a child node by its pre-resolved TSFieldId in O(1) time safely.
 * @param[in] parent The parent
 *        AST node.
 * @param[in] fieldId The pre-resolved field ID.
 * @return The child node, or a null TSNode if not found.
 */
[[nodiscard]] inline TSNode GetChildByField(TSNode parent, TSFieldId fieldId) noexcept
{
    if (ts_node_is_null(parent) || fieldId == 0)
    {
        return TSNode{};
    }
    return ts_node_child_by_field_id(parent, fieldId);
}

/**
 * @brief Retrieves the callee function/method node of a call expression safely.
 * @param[in] callNode The call_expression or construct_call_expression AST node.
 * @return The function node, or a null TSNode if not found.
 */
[[nodiscard]] inline TSNode GetCallCallee(TSNode callNode) noexcept
{
    if (ts_node_is_null(callNode))
    {
        return TSNode{};
    }
    TSNode fn = GetChildByField(callNode, fields::Function);
    if (!ts_node_is_null(fn))
    {
        return fn;
    }
    TSNode typeNode = GetChildByField(callNode, fields::Type);
    if (!ts_node_is_null(typeNode))
    {
        return typeNode;
    }
    if (ts_node_named_child_count(callNode) > 0)
    {
        return ts_node_named_child(callNode, 0);
    }
    return TSNode{};
}

/**
 * @brief Every constant in nodes::, for the test that checks they still resolve.
 */
inline constexpr std::string_view k_allNodeTypes[] = {''')

for n in nodes:
    out.append('    nodes::%s,' % pascal(n))

out.append('''\
};

/**
 * @brief Every constant in fields::, same purpose.
 */
inline constexpr std::string_view k_allFieldNames[] = {''')

for f in fields:
    out.append('    fields::%s,' % pascal(f))

out.append('''\
};
} // namespace angel_lsp::parser
''')

out_path = SERVER / 'src' / 'parser' / 'GrammarNames.h'
out_path.write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
print(f'generated {out_path.relative_to(REPO_ROOT)}: {len(nodes)} node types, {len(fields)} fields')
