#include "analysis/checkers/null_safety/NullSafetyParam.h"

#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>

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
    while (scope && !scope->isFunctionScope && scope->parent)
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

static TSNode FindParameterListNode(TSNode funcNode)
{
    TSNode params = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldParameters);
    if (!ts_node_is_null(params))
    {
        return params;
    }
    const uint32_t c = ts_node_named_child_count(funcNode);
    for (uint32_t i = 0; i < c; ++i)
    {
        TSNode child = ts_node_named_child(funcNode, i);
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(child);
        if (sym == gs.symParameterList || sym == gs.symLambdaParameterList)
        {
            return child;
        }
    }
    return TSNode{};
}

static void CollectRegularParameters(TSNode params, const NullCheckContext& ctx, FlowState& state)
{
    const uint32_t count = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode param = ts_node_named_child(params, i);
        TSNode typeNode = parser::GetChildByField(param, parser::GetGrammarSymbols().fieldParamType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(param, parser::GetGrammarSymbols().fieldType);
        }
        if (ts_node_is_null(typeNode) && ts_node_named_child_count(param) > 0)
        {
            TSNode first = ts_node_named_child(param, 0);
            const auto& gs = parser::GetGrammarSymbols();
            const TSSymbol sym = ts_node_symbol(first);
            if (sym == gs.symType || sym == gs.symPrimitiveType || sym == gs.symScopedIdentifier ||
                sym == gs.symIdentifier)
            {
                typeNode = first;
            }
        }

        if (ts_node_is_null(typeNode))
        {
            continue;
        }

        const auto typeInfo = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode);
        if (!typeInfo.isHandle)
        {
            continue;
        }

        std::string name =
            parser::NodeText(parser::GetChildByField(param, parser::GetGrammarSymbols().fieldName), ctx.sourceCode);
        if (name.empty())
        {
            for (uint32_t j = ts_node_named_child_count(param); j > 0; --j)
            {
                TSNode ch = ts_node_named_child(param, j - 1);
                std::string ident = GetIdentifierName(ch, ctx);
                if (!ident.empty())
                {
                    name = ident;
                    break;
                }
            }
        }
        if (!name.empty())
        {
            state.vars[name] = Nullability::Nullable;
        }
    }
}

static void CollectParametersFromAST(TSNode funcNode, const NullCheckContext& ctx, FlowState& state)
{
    TSNode params = FindParameterListNode(funcNode);
    if (ts_node_is_null(params))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(params) == gs.symLambdaParameterList)
    {
        const auto lambdaParams = ExtractLambdaParameters(params, ctx.sourceCode);
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
    CollectParametersFromAST(funcNode, ctx, state);
}

} // namespace angel_lsp::analysis
