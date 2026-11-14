#include "analysis/index/ValueExpressionFacts.h"

#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Classifies a leaf expression node into a value expression fact.
 * @param[in] node Non-null leaf node with parentheses already removed.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @param[in,out] fact Fact whose kind, span and spelling are filled in.
 */
static void ClassifyLeaf(TSNode node, const NodeIndex* index, std::string_view source, ValueExpressionFact& fact)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol symbol = ts_node_symbol(node);
    fact.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)};
    fact.nodeSymbol = symbol;

    if (symbol == gs.symNumberLiteral)
    {
        fact.kind = ValueExpressionKind::Number;
        fact.text = parser::NodeText(node, source);
    }
    else if (symbol == gs.symStringLiteral)
    {
        fact.kind = ValueExpressionKind::String;
    }
    else if (symbol == gs.symBooleanLiteral)
    {
        fact.kind = ValueExpressionKind::Boolean;
    }
    else if (symbol == gs.symNullLiteral)
    {
        fact.kind = ValueExpressionKind::Null;
    }
    else if (symbol == gs.symIdentifier || symbol == gs.symScopedIdentifier)
    {
        fact.kind = ValueExpressionKind::Identifier;
        fact.text = parser::NodeText(node, source);
    }
    else if (symbol == gs.symCallExpression || symbol == gs.symConstructCallExpression)
    {
        const TSNode callee = SyntaxOperands(node, index).Callee();
        if (!ts_node_is_null(callee))
        {
            fact.kind = ValueExpressionKind::Call;
            fact.text = parser::NodeText(callee, source);
        }
    }
    else if (symbol == gs.symCastExpression || symbol == gs.symFunctionalCastExpression)
    {
        fact.kind = ValueExpressionKind::Cast;
        fact.text = parser::NodeText(SyntaxOperands(node, index).Field(gs.fieldType), source);
    }
    else if (symbol == gs.symMemberExpression)
    {
        fact.kind = ValueExpressionKind::Member;
    }
    else if (symbol == gs.symIndexExpression)
    {
        fact.kind = ValueExpressionKind::Index;
    }
}

ValueExpressionFact ReadValueExpression(TSNode node, const NodeIndex* index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    ValueExpressionFact fact;
    for (;;)
    {
        node = UnwrapIndexedParentheses(node, index);
        if (ts_node_is_null(node))
        {
            return fact;
        }
        if (ts_node_is_error(node) || ts_node_is_missing(node) || (index && index->NodeHasSyntaxError(node)))
        {
            fact.kind = ValueExpressionKind::Poison;
            return fact;
        }
        if (ts_node_symbol(node) != gs.symUnaryExpression)
        {
            ClassifyLeaf(node, index, source, fact);
            return fact;
        }

        const SyntaxOperands syntax(node, index);
        const std::string_view op = parser::NodeText(syntax.Field(gs.fieldOperator), source);
        if (op == "!" || op == "not")
        {
            fact.logicalNot = true;
            return fact;
        }
        fact.negated = fact.negated || op == "-";
        node = syntax.Field(gs.fieldOperand);
    }
}

} // namespace angel_lsp::analysis
