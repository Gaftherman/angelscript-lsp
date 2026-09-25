#include "analysis/NullSafetyDecl.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/NullSafetyExpr.h"
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

std::string CleanTypeName(std::string_view typeStr)
{
    while (!typeStr.empty() && (typeStr.front() == ' ' || typeStr.front() == '\t'))
    {
        typeStr.remove_prefix(1);
    }
    if (typeStr.starts_with("const "))
    {
        typeStr.remove_prefix(6);
    }
    while (!typeStr.empty() &&
           (typeStr.back() == '@' || typeStr.back() == '&' || typeStr.back() == ' ' || typeStr.back() == '\t'))
    {
        typeStr.remove_suffix(1);
    }
    return std::string(typeStr);
}

bool IsKnownNonNullInit(TSNode val, TSNode typeNode, std::string_view sourceCode)
{
    if (ts_node_is_null(val))
    {
        return false;
    }
    TSNode unwrapped = UnwrapNullExpression(val);
    if (ts_node_is_null(unwrapped))
    {
        return false;
    }
    std::string_view valType = ts_node_type(unwrapped);
    if (valType == parser::nodes::ConstructCallExpression || valType == parser::nodes::ThisExpression)
    {
        return true;
    }
    if (valType == parser::nodes::CallExpression && !ts_node_is_null(typeNode))
    {
        TSNode fn = parser::GetChildByField(unwrapped, parser::fields::Function);
        std::string fnName = GetIdentifierName(fn, sourceCode);
        std::string cleanType = CleanTypeName(NodeText(typeNode, sourceCode));
        if (!fnName.empty() && fnName == cleanType)
        {
            return true;
        }
    }
    return false;
}
} // namespace

void CheckVarDeclaration(TSNode stmt, FlowState& state, NullCheckContext& ctx)
{
    TSNode typeNode = parser::GetChildByField(stmt, parser::fields::VarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(stmt, parser::fields::Type);
    }
    std::string stmtText = NodeText(stmt, ctx.sourceCode);
    const auto eqPos = stmtText.find('=');
    std::string declPrefix = (eqPos != std::string::npos) ? stmtText.substr(0, eqPos) : stmtText;
    bool isHandle = (!ts_node_is_null(typeNode) && NodeText(typeNode, ctx.sourceCode).find('@') != std::string::npos) ||
                    (declPrefix.find('@') != std::string::npos);

    const uint32_t childCount = ts_node_named_child_count(stmt);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_named_child(stmt, i);
        if (std::string_view(ts_node_type(child)) != parser::nodes::VariableDeclarator)
        {
            continue;
        }
        std::string name = NodeText(parser::GetChildByField(child, parser::fields::Name), ctx.sourceCode);
        TSNode val = parser::GetChildByField(child, parser::fields::Value);
        if (ts_node_is_null(val))
        {
            val = parser::GetChildByField(child, parser::fields::Init);
        }
        CheckNullExpression(val, state, ctx, 0);

        if (isHandle && !name.empty())
        {
            if (ts_node_is_null(val) || NodeText(val, ctx.sourceCode) == "null")
            {
                state.vars[name] = Nullability::DefinitelyNull;
            }
            else if (IsKnownNonNullInit(val, typeNode, ctx.sourceCode))
            {
                state.vars[name] = Nullability::NonNull;
            }
            else
            {
                state.vars[name] = Nullability::Nullable;
            }
        }
    }
}

void CheckAssignmentStmt(TSNode expr, FlowState& state, NullCheckContext& ctx)
{
    TSNode left = parser::GetChildByField(expr, parser::fields::Left);
    TSNode right = parser::GetChildByField(expr, parser::fields::Right);
    CheckNullExpression(right, state, ctx, 0);

    TSNode unwrappedLeft = UnwrapNullExpression(left);
    std::string name = GetIdentifierName(unwrappedLeft, ctx.sourceCode);
    if (!name.empty() && state.vars.contains(name))
    {
        state.vars[name] = (NodeText(right, ctx.sourceCode) == "null") ? Nullability::DefinitelyNull
                                                                       : Nullability::Nullable;
    }
    else
    {
        CheckNullExpression(left, state, ctx, 0);
    }
}

} // namespace angel_lsp::analysis
