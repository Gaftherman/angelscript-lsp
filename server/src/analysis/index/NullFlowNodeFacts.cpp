#include "analysis/index/NullFlowFactsInternal.h"
#include "analysis/index/Symbol.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Maps a Tree-Sitter grammar symbol to its corresponding flow analysis node kind.
 * @param[in] symbol Grammar symbol of the AST node.
 * @param[in] gs Pre-resolved grammar symbols cache.
 * @return Semantic node kind recognized by null flow analysis.
 */
static NullFlowNodeKind MapNodeKind(TSSymbol symbol, const parser::GrammarSymbols& gs) noexcept
{
    if (symbol == gs.symStatementBlock)
    {
        return NullFlowNodeKind::Block;
    }
    if (symbol == gs.symVariableDeclaration)
    {
        return NullFlowNodeKind::VariableDeclaration;
    }
    if (symbol == gs.symExpressionStatement)
    {
        return NullFlowNodeKind::ExpressionStatement;
    }
    if (symbol == gs.symIfStatement)
    {
        return NullFlowNodeKind::IfStatement;
    }
    if (symbol == gs.symReturnStatement)
    {
        return NullFlowNodeKind::ReturnStatement;
    }
    if (symbol == gs.symIdentifier)
    {
        return NullFlowNodeKind::Identifier;
    }
    if (symbol == gs.symScopedIdentifier)
    {
        return NullFlowNodeKind::ScopedIdentifier;
    }
    if (symbol == gs.symParenthesizedExpression)
    {
        return NullFlowNodeKind::Parenthesized;
    }
    if (symbol == gs.symUnaryExpression)
    {
        return NullFlowNodeKind::Unary;
    }
    if (symbol == gs.symAssignmentExpression)
    {
        return NullFlowNodeKind::Assignment;
    }
    if (symbol == gs.symBinaryExpression)
    {
        return NullFlowNodeKind::Binary;
    }
    if (symbol == gs.symMemberExpression)
    {
        return NullFlowNodeKind::Member;
    }
    if (symbol == gs.symIndexExpression)
    {
        return NullFlowNodeKind::Index;
    }
    if (symbol == gs.symCallExpression)
    {
        return NullFlowNodeKind::Call;
    }
    if (symbol == gs.symCastExpression)
    {
        return NullFlowNodeKind::Cast;
    }
    if (symbol == gs.symTernaryExpression)
    {
        return NullFlowNodeKind::Ternary;
    }
    if (symbol == gs.symConstructCallExpression)
    {
        return NullFlowNodeKind::ConstructCall;
    }
    if (symbol == gs.symThisExpression)
    {
        return NullFlowNodeKind::This;
    }
    return NullFlowNodeKind::Other;
}

/**
 * @brief Classifies a binary operator token node into a supported null flow operator.
 * @param[in] opNode Operator token AST node.
 * @param[in] gs Pre-resolved grammar symbols cache.
 * @return Recognized binary operator enum value.
 */
static NullFlowOperator MapBinaryOperator(TSNode opNode, const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(opNode))
    {
        return NullFlowOperator::None;
    }
    const TSSymbol symbol = ts_node_symbol(opNode);
    if (symbol == gs.tokIs)
    {
        return NullFlowOperator::Is;
    }
    if (symbol == gs.tokEqEq)
    {
        return NullFlowOperator::EqEq;
    }
    if (symbol == gs.tokBangIs)
    {
        return NullFlowOperator::NotIs;
    }
    if (symbol == gs.tokBangEq)
    {
        return NullFlowOperator::NotEq;
    }
    if (symbol == gs.tokAmpAmp || symbol == gs.tokAnd)
    {
        return NullFlowOperator::And;
    }
    if (symbol == gs.tokPipePipe || symbol == gs.tokOr)
    {
        return NullFlowOperator::Or;
    }
    return NullFlowOperator::Other;
}

/**
 * @brief Allocates an arena node populated with syntax metadata and operator classifications.
 * @param[in] node AST node to capture; a null node yields NoNullFlowNode.
 * @return Assigned arena id for the newly appended node, or NoNullFlowNode when node is null.
 */
NullFlowNodeId NullFlowFactsBuilder::Declare(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return NoNullFlowNode;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol symbol = ts_node_symbol(node);

    NullFlowNode item;
    item.range = ToSourceRange(node);
    item.text = parser::NodeText(node, m_source);
    item.isNullInitializer = IsNullInitializer(node, &m_index);
    item.kind = MapNodeKind(symbol, gs);

    if (item.kind == NullFlowNodeKind::Binary)
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode opNode = syntax.Field(gs.fieldOperator);
        item.op = MapBinaryOperator(opNode, gs);
        item.complete = !ts_node_is_null(opNode) && !ts_node_is_null(syntax.Field(gs.fieldLeft)) &&
                        !ts_node_is_null(syntax.Field(gs.fieldRight));
    }
    else if (item.kind == NullFlowNodeKind::Unary)
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode opNode = syntax.Field(gs.fieldOperator);
        const TSNode operandNode = syntax.Field(gs.fieldOperand);

        if (syntax.HasUnaryOperator(gs.tokBang) || syntax.HasUnaryOperator(gs.tokNot))
        {
            item.op = NullFlowOperator::Not;
        }
        else if (!ts_node_is_null(opNode))
        {
            item.op = NullFlowOperator::Other;
        }
        else
        {
            item.op = NullFlowOperator::None;
        }

        item.singleCharOperator = !ts_node_is_null(opNode) && !ts_node_is_null(operandNode) &&
                                  (ts_node_end_byte(opNode) - ts_node_start_byte(opNode) == 1);
    }

    const auto id = static_cast<NullFlowNodeId>(m_facts.nodes.size());
    m_facts.nodes.push_back(item);
    return id;
}

} // namespace angel_lsp::analysis
