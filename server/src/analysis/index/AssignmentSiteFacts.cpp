#include "analysis/index/AssignmentSiteFacts.h"

#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

const AssignmentSiteFact* AssignmentSiteFacts::Find(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(assignments, bytes);
}

/**
 * @brief Converts a node's start and end points to a diagnostic range.
 * @param[in] node Non-null syntax node.
 * @return Line and column span of the node.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

/**
 * @brief Converts a node to its byte span.
 * @param[in] node Non-null syntax node.
 * @return Start and end byte of the node.
 */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    return {ts_node_start_byte(node), ts_node_end_byte(node)};
}

/**
 * @brief Removes a leading unary `@` from an assignment operand.
 * @param[in] operand Operand expression node.
 * @param[in] index Precomputed node index of the document.
 * @param[out] isHandle Set to true when the operand carried the `@`; left untouched otherwise.
 * @return The operand of the `@`, or the node itself when it has none.
 */
static TSNode UnwrapHandleOperand(TSNode operand, const NodeIndex& index, bool& isHandle)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(operand, &index);
    if (!syntax.HasUnaryOperator(gs.tokAt))
    {
        return operand;
    }
    isHandle = true;
    const TSNode inner = syntax.Field(gs.fieldOperand);
    return ts_node_is_null(inner) ? operand : inner;
}

FuncdefValueFact ReadFuncdefValue(TSNode valueNode, const NodeIndex& index, std::string_view source)
{
    FuncdefValueFact fact;
    if (ts_node_is_null(valueNode))
    {
        return fact;
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSNode actual = valueNode;
    if (ts_node_symbol(valueNode) == gs.symUnaryExpression)
    {
        const SyntaxOperands syntax(valueNode, &index);
        const TSNode op = syntax.Field(gs.fieldOperator);
        const TSNode operand = syntax.Field(gs.fieldOperand);
        if (!ts_node_is_null(op) && !ts_node_is_null(operand) &&
            (ts_node_symbol(op) == gs.tokAt || parser::NodeText(op, source) == "@"))
        {
            actual = operand;
        }
    }

    fact.present = true;
    fact.text = parser::NodeText(actual, source);
    TrimTypeWhitespace(fact.text);
    fact.isLambda = ts_node_symbol(actual) == gs.symLambdaExpression;
    if (!fact.isLambda)
    {
        return fact;
    }
    const TSNode list = SyntaxOperands(actual, &index).Field(gs.fieldParameters);
    fact.hasParameterList = !ts_node_is_null(list);
    if (fact.hasParameterList)
    {
        fact.lambdaParameters = ReadLambdaParameters(list, source);
    }
    return fact;
}

/**
 * @brief Captures one assignment whose operands are present.
 * @param[in] node Assignment expression node.
 * @param[in] left Left operand node.
 * @param[in] right Right operand node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @return Assignment fact for the node.
 */
static AssignmentSiteFact ReadAssignment(TSNode node, TSNode left, TSNode right, const NodeIndex& index,
                                         std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    AssignmentSiteFact fact;
    fact.bytes = BytesOf(node);
    fact.range = RangeOf(node);
    fact.left = BuildBinaryOperandFact(left, index, source);
    fact.right = BuildBinaryOperandFact(right, index, source);

    const TSNode op = syntax.Field(gs.fieldOperator);
    if (!ts_node_is_null(op))
    {
        fact.op = parser::NodeText(op, source);
    }
    fact.opIsAssign = ts_node_symbol(op) == gs.tokAssign;

    fact.leftValue = ReadValueExpression(UnwrapHandleOperand(left, index, fact.leftIsHandle), &index, source);
    fact.rightValue = ReadValueExpression(UnwrapHandleOperand(right, index, fact.rightIsHandle), &index, source);
    fact.rightIsNull = ts_node_symbol(UnwrapIndexedParentheses(right, &index)) == gs.symNullLiteral;
    fact.rightFuncdef = ReadFuncdefValue(right, index, source);

    fact.leftIsIndex = ts_node_symbol(left) == gs.symIndexExpression;
    const TSNode propertyNode = fact.leftIsIndex ? SyntaxOperands(left, &index).FieldOrNamed(gs.fieldObject) : left;
    fact.leftProperty = ReadPropertyAccess(propertyNode, &index, source);
    return fact;
}

AssignmentSiteFacts BuildAssignmentSiteFacts(const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    AssignmentSiteFacts facts;
    for (const TSNode node : index.Nodes(gs.symAssignmentExpression))
    {
        const SyntaxOperands syntax(node, &index);
        const TSNode left = syntax.Field(gs.fieldLeft);
        const TSNode right = syntax.Field(gs.fieldRight);
        if (ts_node_is_null(left) || ts_node_is_null(right))
        {
            continue;
        }
        facts.assignments.push_back(ReadAssignment(node, left, right, index, source));
    }
    SortBySpan(facts.assignments);
    return facts;
}

} // namespace angel_lsp::analysis
