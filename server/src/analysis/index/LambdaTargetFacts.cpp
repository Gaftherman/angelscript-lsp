#include "analysis/index/LambdaTargetFacts.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Reads the declared type of the declaration that owns a variable declarator.
 * @param[in] declarator Variable declarator holding the lambda initializer.
 * @param[in] source Document text.
 * @return DeclaratorType fact, or an empty fact when the declaration has no type node.
 */
static LambdaTargetFact ReadDeclaratorTarget(TSNode declarator, std::string_view source)
{
    const TSNode declaration = parser::GetParent(declarator);
    if (ts_node_is_null(declaration))
    {
        return {};
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode typeNode = parser::GetChildByField(declaration, gs.fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(declaration, gs.fieldType);
    }
    if (ts_node_is_null(typeNode))
    {
        return {};
    }
    return {.kind = LambdaTargetKind::DeclaratorType, .text = parser::NodeText(typeNode, source)};
}

/**
 * @brief Reads the assigned expression of an assignment whose right side is the lambda.
 * @param[in] assignment Assignment expression node.
 * @param[in] source Document text.
 * @return AssignmentLeft fact, or an empty fact when the left side is missing.
 */
static LambdaTargetFact ReadAssignmentTarget(TSNode assignment, std::string_view source)
{
    const TSNode left = parser::GetChildByField(assignment, parser::GetGrammarSymbols().fieldLeft);
    if (ts_node_is_null(left))
    {
        return {};
    }
    return {.kind = LambdaTargetKind::AssignmentLeft, .text = parser::NodeText(left, source)};
}

/**
 * @brief Reads the callee and argument position of a call whose argument is the lambda.
 * @param[in] argumentList Argument list node that contains the lambda.
 * @param[in] lambdaNode Lambda expression node.
 * @param[in] source Document text.
 * @return CallArgument fact, or an empty fact when the lambda, call or callee cannot be located.
 */
static LambdaTargetFact ReadCallArgumentTarget(TSNode argumentList, TSNode lambdaNode, std::string_view source)
{
    const uint32_t count = ts_node_named_child_count(argumentList);
    for (uint32_t position = 0; position < count; ++position)
    {
        if (!ts_node_eq(ts_node_named_child(argumentList, position), lambdaNode))
        {
            continue;
        }
        const TSNode call = parser::GetParent(argumentList);
        if (ts_node_is_null(call))
        {
            return {};
        }
        const TSNode callee = parser::GetCallCallee(call);
        if (ts_node_is_null(callee))
        {
            return {};
        }
        return {.kind = LambdaTargetKind::CallArgument,
                .text = parser::NodeText(callee, source),
                .position = position,
                .soleArgument = position == 0 && count == 1};
    }
    return {};
}

LambdaTargetFact ReadLambdaTarget(TSNode lambdaNode, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(lambdaNode) || ts_node_symbol(lambdaNode) != gs.symLambdaExpression)
    {
        return {};
    }
    const TSNode parent = parser::GetParent(lambdaNode);
    if (ts_node_is_null(parent))
    {
        return {};
    }
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symVariableDeclarator)
    {
        return ReadDeclaratorTarget(parent, source);
    }
    if (parentSym == gs.symAssignmentExpression)
    {
        return ReadAssignmentTarget(parent, source);
    }
    if (parentSym == gs.symArgumentList)
    {
        return ReadCallArgumentTarget(parent, lambdaNode, source);
    }
    return {};
}

} // namespace angel_lsp::analysis
