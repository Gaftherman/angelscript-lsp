#include "analysis/NullSafetyParam.h"
#include "analysis/NullSafetyCondition.h"
#include "parser/GrammarNames.h"
#include <string>

namespace angel_lsp::analysis
{
namespace
{
std::string NodeText(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return "";
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return "";
    }
    return std::string(sourceCode.substr(start, end - start));
}

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
            if (def.kind == LocalDefinitionKind::Parameter &&
                (def.isHandleType || def.typeName.find('@') != std::string::npos))
            {
                state.vars[def.name] = Nullability::Nullable;
            }
        }
    }
}

TSNode FindParameterListNode(TSNode funcNode)
{
    TSNode params = parser::GetChildByField(funcNode, parser::fields::Parameters);
    if (!ts_node_is_null(params))
    {
        return params;
    }
    const uint32_t c = ts_node_named_child_count(funcNode);
    for (uint32_t i = 0; i < c; ++i)
    {
        TSNode child = ts_node_named_child(funcNode, i);
        std::string_view childType = ts_node_type(child);
        if (childType == parser::nodes::ParameterList || childType == parser::nodes::LambdaParameterList)
        {
            return child;
        }
    }
    return TSNode{};
}

void CollectParametersFromAST(TSNode funcNode, std::string_view sourceCode, FlowState& state)
{
    TSNode params = FindParameterListNode(funcNode);
    if (ts_node_is_null(params))
    {
        return;
    }

    const uint32_t count = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode param = ts_node_named_child(params, i);
        if (NodeText(param, sourceCode).find('@') == std::string::npos)
        {
            continue;
        }
        std::string name = NodeText(parser::GetChildByField(param, parser::fields::Name), sourceCode);
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
} // namespace

TSNode FindFunctionBody(TSNode funcNode)
{
    TSNode body = parser::GetChildByField(funcNode, parser::fields::Body);
    if (!ts_node_is_null(body) && std::string_view(ts_node_type(body)) == parser::nodes::StatementBlock)
    {
        return body;
    }
    const uint32_t count = ts_node_named_child_count(funcNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(funcNode, i);
        if (std::string_view(ts_node_type(child)) == parser::nodes::StatementBlock)
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
