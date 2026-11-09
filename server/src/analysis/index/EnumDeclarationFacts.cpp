#include "analysis/index/EnumDeclarationFacts.h"

#include "analysis/index/CodeActionFacts.h"
#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/NodeIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Extracts all direct enum member declarations from an enum declaration AST node.
 * @param[in] enumNode AST node of the enum declaration.
 * @param[in] source Document source text buffer.
 * @return Vector of extracted enum members in declaration order.
 */
static std::vector<IndexedEnumMember> ExtractEnumMembers(TSNode enumNode, std::string_view source)
{
    if (ts_node_is_null(enumNode))
    {
        return {};
    }

    const auto& gs = parser::GetGrammarSymbols();
    std::vector<IndexedEnumMember> members;
    TSTreeCursor cursor = ts_tree_cursor_new(enumNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(child) != gs.symEnumMember)
            {
                continue;
            }

            const TSNode memberNameNode = parser::GetChildByField(child, gs.fieldName);
            if (ts_node_is_null(memberNameNode))
            {
                continue;
            }

            const std::string_view memberName = parser::NodeText(memberNameNode, source);
            if (memberName.empty())
            {
                continue;
            }

            members.push_back(IndexedEnumMember{memberName, MakeIndexedSourceSpan(memberNameNode)});
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);

    return members;
}

/**
 * @brief Extracts AST-free facts for all enum declarations in the document.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] source Document source text buffer.
 * @return Collection of extracted enum declarations in syntactic declaration order.
 */
std::vector<IndexedEnumDeclaration> BuildEnumDeclarationFacts(const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::span<const TSNode> enumNodes = index.Nodes(gs.symEnumDeclaration);
    if (enumNodes.empty())
    {
        return {};
    }

    std::vector<IndexedEnumDeclaration> declarations;
    declarations.reserve(enumNodes.size());

    for (const TSNode enumNode : enumNodes)
    {
        if (ts_node_is_null(enumNode))
        {
            continue;
        }

        const TSNode nameNode = parser::GetChildByField(enumNode, gs.fieldName);
        const std::string_view name = parser::NodeText(nameNode, source);
        const TSPoint startPoint = ts_node_start_point(enumNode);
        const IndexedSourcePoint start{startPoint.row, startPoint.column};

        declarations.push_back(IndexedEnumDeclaration{name, start, ExtractEnumMembers(enumNode, source)});
    }

    return declarations;
}

} // namespace angel_lsp::analysis
