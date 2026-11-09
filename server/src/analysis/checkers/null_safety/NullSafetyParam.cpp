#include "analysis/checkers/null_safety/NullSafetyParam.h"

#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>
#include <utility>

namespace angel_lsp::analysis
{
static void CollectParametersFromScope(TSNode bodyNode, const Scope* scopeRoot, FlowState& state)
{
    if (!scopeRoot || ts_node_is_null(bodyNode))
    {
        return;
    }
    const TSPoint pt = ts_node_start_point(bodyNode);
    const Scope* scope = FindInnermostScope(scopeRoot, pt.row, pt.column);
    ScopeTraversalGuard guard;
    while (scope && !scope->isFunctionScope && scope->parent && guard.CheckAndInsert(scope))
    {
        scope = scope->parent;
    }
    if (scope)
    {
        for (const auto& def : scope->definitions)
        {
            if (def.kind == LocalDefinitionKind::Parameter && def.isHandleType)
            {
                state.vars[def.name] = Nullability::Nullable;
            }
        }
    }
}

static TSNode FindParameterListNode(TSNode funcNode, const NodeIndex& index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands callable(funcNode, &index);
    TSNode params = callable.Field(gs.fieldParameters);
    if (ts_node_is_null(params))
    {
        callable.ForEachNamed(
            [&](TSNode child)
            {
                const auto symbol = ts_node_symbol(child);
                if (symbol == gs.symParameterList || symbol == gs.symLambdaParameterList)
                {
                    params = child;
                    return false;
                }
                return true;
            });
    }
    return params;
}

static TSNode ReadParameterType(const SyntaxOperands& parameter)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode type = parameter.Field(gs.fieldParamType);
    if (!ts_node_is_null(type))
    {
        return type;
    }
    const TSNode fallbackField = parameter.Field(gs.fieldType);
    if (!ts_node_is_null(fallbackField))
    {
        return fallbackField;
    }
    const TSNode recovered = parameter.FirstNamed();
    if (ts_node_is_null(recovered))
    {
        return {};
    }
    const auto symbol = ts_node_symbol(recovered);
    return symbol == gs.symType || symbol == gs.symPrimitiveType || symbol == gs.symScopedIdentifier ||
                   symbol == gs.symIdentifier
               ? recovered
               : TSNode{};
}

static std::string ReadParameterName(const SyntaxOperands& parameter, const NullCheckContext& ctx)
{
    std::string name(parser::NodeText(parameter.Field(parser::GetGrammarSymbols().fieldName), ctx.sourceCode));
    if (name.empty())
    {
        parameter.ForEachNamed(
            [&](TSNode child)
            {
                auto identifier = GetIdentifierName(child, ctx);
                if (!identifier.empty())
                {
                    name = std::move(identifier);
                }
                return true;
            });
    }
    return name;
}

static void CollectRegularParameters(TSNode params, const NullCheckContext& ctx, FlowState& state)
{
    SyntaxOperands(params, &ctx.index)
        .ForEachNamedOperands(
            [&](const SyntaxOperands& operands)
            {
                const auto type = ExtractTypeInfoFromAST(ReadParameterType(operands), ctx.sourceCode, &ctx.index);
                if (type.isHandle)
                {
                    auto name = ReadParameterName(operands, ctx);
                    if (!name.empty())
                    {
                        state.vars[std::move(name)] = Nullability::Nullable;
                    }
                }
                return true;
            });
}

static void CollectIndexedParameters(TSNode funcNode, const NullCheckContext& ctx, FlowState& state)
{
    TSNode params = FindParameterListNode(funcNode, ctx.index);
    if (ts_node_is_null(params))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(params) == gs.symLambdaParameterList)
    {
        const auto lambdaParams = ExtractLambdaParameters(params, ctx.sourceCode, &ctx.index);
        for (const auto& lp : lambdaParams)
        {
            if (lp.isHandle && !lp.name.empty())
            {
                state.vars[lp.name] = Nullability::Nullable;
            }
        }
        return;
    }

    CollectRegularParameters(params, ctx, state);
}

TSNode FindFunctionBody(TSNode funcNode, const NodeIndex& index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(funcNode, &index);
    const TSNode body = syntax.Field(gs.fieldBody);
    if (!ts_node_is_null(body) && ts_node_symbol(body) == gs.symStatementBlock)
    {
        return body;
    }
    return syntax.FirstNamed(gs.symStatementBlock);
}

void CollectParameters(TSNode funcNode, TSNode body, const NullCheckContext& ctx, FlowState& state)
{
    CollectParametersFromScope(body, ctx.scopeRoot, state);
    CollectIndexedParameters(funcNode, ctx, state);
}

} // namespace angel_lsp::analysis
