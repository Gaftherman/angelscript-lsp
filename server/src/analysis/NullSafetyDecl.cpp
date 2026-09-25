#include "analysis/NullSafetyDecl.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/NullSafetyExpr.h"
#include "analysis/SymbolTable.h"
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

bool IsTargetVariableTypeMatch(std::string_view targetVarName, std::string_view typeName, NullCheckContext& ctx)
{
    if (targetVarName.empty() || typeName.empty())
    {
        return false;
    }
    const auto& table = ctx.diagCtx.request.symbolTable;
    if (const auto symPtr = table.FindSymbolsPtr(std::string(targetVarName)))
    {
        for (const auto& sym : *symPtr)
        {
            if (sym.type == SymbolType::Variable)
            {
                const auto& varSig = sym.GetVariable();
                if (varSig.baseTypeName == typeName || varSig.typeName == typeName)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool IsConstructorCall(std::string_view fnName, TSNode typeNode, std::string_view targetVarName,
                       NullCheckContext& ctx)
{
    if (fnName.empty())
    {
        return false;
    }
    const std::string shortName =
        (fnName.rfind("::") != std::string::npos) ? std::string(fnName.substr(fnName.rfind("::") + 2))
                                                  : std::string(fnName);
    if (shortName.empty())
    {
        return false;
    }

    if (!ts_node_is_null(typeNode))
    {
        const auto typeInfo = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode);
        if (shortName == typeInfo.baseTypeName)
        {
            return true;
        }
    }

    if (IsTargetVariableTypeMatch(targetVarName, shortName, ctx))
    {
        return true;
    }

    const auto typeSyms = ctx.diagCtx.request.symbolTable.FindTypeSymbolsByShortName(shortName);
    for (const auto& sym : typeSyms)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
        {
            return true;
        }
    }

    return ctx.diagCtx.request.IsRegisteredSymbol(shortName);
}

struct NullableEvalRequest
{
    TSNode val;
    TSNode typeNode = {0, 0, 0, 0};
    std::string_view targetVarName = "";
};

Nullability EvaluateExpressionNullability(const NullableEvalRequest& req, const FlowState& state,
                                         NullCheckContext& ctx)
{
    if (ts_node_is_null(req.val))
    {
        return Nullability::DefinitelyNull;
    }
    TSNode unwrapped = UnwrapNullExpression(req.val);
    if (ts_node_is_null(unwrapped) || IsNullInitializer(unwrapped))
    {
        return Nullability::DefinitelyNull;
    }

    std::string_view valType = ts_node_type(unwrapped);
    if (valType == parser::nodes::ConstructCallExpression || valType == parser::nodes::ThisExpression)
    {
        return Nullability::NonNull;
    }

    const std::string rhsVarName = GetIdentifierName(unwrapped, ctx.sourceCode);
    if (!rhsVarName.empty())
    {
        auto it = state.vars.find(rhsVarName);
        if (it != state.vars.end())
        {
            return it->second;
        }
    }

    if (valType == parser::nodes::CallExpression)
    {
        TSNode fn = parser::GetChildByField(unwrapped, parser::fields::Function);
        std::string fnName = GetIdentifierName(fn, ctx.sourceCode);
        if (fnName.empty() && !ts_node_is_null(fn))
        {
            fnName = NodeText(fn, ctx.sourceCode);
        }
        if (IsConstructorCall(fnName, req.typeNode, req.targetVarName, ctx))
        {
            return Nullability::NonNull;
        }
    }

    return Nullability::Nullable;
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
            state.vars[name] = EvaluateExpressionNullability({val, typeNode, name}, state, ctx);
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
    if (!name.empty())
    {
        const Nullability assignedNullability =
            EvaluateExpressionNullability({right, {}, name}, state, ctx);
        if (assignedNullability != Nullability::Nullable || state.vars.contains(name))
        {
            state.vars[name] = assignedNullability;
        }
    }
    else
    {
        CheckNullExpression(left, state, ctx, 0);
    }
}

} // namespace angel_lsp::analysis
