#include "analysis/index/ReflectionCallFacts.h"

#include "analysis/index/CallArgumentFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Concatenates the unquoted string literals of an argument expression, left to right.
 * @param[in] exprNode Argument expression; string literals and the operands of binary expressions contribute.
 * @param[in] source Document text.
 * @return The joined literal text; empty when the expression holds no string literal.
 */
static std::string ExtractStringValue(TSNode exprNode, std::string_view source)
{
    std::string result;
    if (ts_node_is_null(exprNode))
    {
        return result;
    }

    const auto& gs = parser::GetGrammarSymbols();
    std::vector<TSNode> stack{exprNode};
    while (!stack.empty())
    {
        const TSNode node = stack.back();
        stack.pop_back();
        if (ts_node_is_null(node))
        {
            continue;
        }
        const TSSymbol sym = ts_node_symbol(node);
        if (sym == gs.symStringLiteral)
        {
            result += parser::UnquoteStringView(parser::NodeText(node, source));
        }
        else if (sym == gs.symBinaryExpression)
        {
            stack.push_back(parser::GetChildByField(node, gs.fieldRight));
            stack.push_back(parser::GetChildByField(node, gs.fieldLeft));
        }
    }
    return result;
}

std::vector<IndexedReflectionCall> BuildReflectionCallFacts(const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<IndexedReflectionCall> calls;
    for (const TSNode callNode : index.Nodes(gs.symCallExpression))
    {
        const TSNode funcNode = parser::GetChildByField(callNode, gs.fieldFunction);
        if (ts_node_is_null(funcNode))
        {
            continue;
        }

        IndexedReflectionCall call;
        call.calleeText = parser::NodeText(funcNode, source);
        for (const CallArgumentInfo& arg : ExtractCallArguments(callNode, source))
        {
            call.argStringValues.push_back(ExtractStringValue(arg.exprNode, source));
        }
        calls.push_back(std::move(call));
    }
    return calls;
}

} // namespace angel_lsp::analysis
