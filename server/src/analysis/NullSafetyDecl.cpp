#include "analysis/NullSafetyDecl.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/NullSafetyExpr.h"
#include "analysis/TypeExtraction.h"
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
        const auto typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        if (!fnName.empty() && fnName == typeInfo.baseTypeName)
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
    const auto typeInfo = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode);
    const bool isHandle = typeInfo.isHandle;

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
            if (ts_node_is_null(val) || IsNullInitializer(val))
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
        state.vars[name] = IsNullInitializer(right) ? Nullability::DefinitelyNull
                                                    : Nullability::Nullable;
    }
    else
    {
        CheckNullExpression(left, state, ctx, 0);
    }
}

} // namespace angel_lsp::analysis
