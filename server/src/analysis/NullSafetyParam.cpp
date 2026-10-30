#include "analysis/NullSafetyParam.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include <string>

namespace angel_lsp::analysis
{
namespace
{
void CollectParametersFromScope(TSNode bodyNode, const Scope* scopeRoot, FlowState& state)
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

TSNode FindParameterListNode(TSNode funcNode)
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

void CollectRegularParameters(TSNode params, std::string_view sourceCode, FlowState& state)
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

        const auto typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        if (!typeInfo.isHandle)
        {
            continue;
        }

        std::string name = NodeText(parser::GetChildByField(param, parser::GetGrammarSymbols().fieldName), sourceCode);
        if (name.empty())
        {
            for (uint32_t j = ts_node_named_child_count(param); j > 0; --j)
            {
                TSNode ch = ts_node_named_child(param, j - 1);
                std::string ident = GetIdentifierName(ch, sourceCode);
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

void CollectParametersFromAST(TSNode funcNode, std::string_view sourceCode, FlowState& state)
{
    TSNode params = FindParameterListNode(funcNode);
    if (ts_node_is_null(params))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(params) == gs.symLambdaParameterList)
    {
        const auto lambdaParams = ExtractLambdaParameters(params, sourceCode);
        for (const auto& lp : lambdaParams)
        {
            if (lp.isHandle && !lp.name.empty())
            {
                state.vars[lp.name] = Nullability::Nullable;
            }
        }
        return;
    }

    CollectRegularParameters(params, sourceCode, state);
}
} // namespace

TSNode FindFunctionBody(TSNode funcNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode body = parser::GetChildByField(funcNode, gs.fieldBody);
    if (!ts_node_is_null(body) && ts_node_symbol(body) == gs.symStatementBlock)
    {
        return body;
    }
    const uint32_t count = ts_node_named_child_count(funcNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(funcNode, i);
        if (ts_node_symbol(child) == gs.symStatementBlock)
        {
            return child;
        }
    }
    return TSNode{};
}

void CollectParameters(TSNode funcNode, const Scope* scopeRoot, std::string_view sourceCode, FlowState& state)
{
    CollectParametersFromScope(FindFunctionBody(funcNode), scopeRoot, state);
    CollectParametersFromAST(funcNode, sourceCode, state);
}

} // namespace angel_lsp::analysis
