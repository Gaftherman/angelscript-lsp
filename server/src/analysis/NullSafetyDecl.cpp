#include "analysis/NullSafetyDecl.h"
#include "analysis/ASTUtils.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/NullSafetyExpr.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "analysis/TypeExtraction.h"
#include "parser/GrammarNames.h"
#include <string>

namespace angel_lsp::analysis
{
namespace
{
bool IsTargetVariableTypeMatch(std::string_view targetVarName, std::string_view typeName, NullCheckContext& ctx)
{
    if (targetVarName.empty() || typeName.empty())
    {
        return false;
    }
    const auto& table = ctx.diagCtx.request.symbolTable;
    if (const auto symPtr = table.FindSymbolsPtr(targetVarName))
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

bool IsConstructorCall(std::string_view fnName, TSNode typeNode, std::string_view targetVarName, NullCheckContext& ctx)
{
    if (fnName.empty())
    {
        return false;
    }
    const std::string shortName = std::string(LastScopeSegment(fnName));
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
    TSNode typeNode{};
    std::string_view targetVarName = "";
};

Nullability EvaluateExpressionNullability(const NullableEvalRequest& req, const FlowState& state, NullCheckContext& ctx)
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

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(unwrapped);
    if (sym == gs.symConstructCallExpression || sym == gs.symThisExpression)
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

    if (sym == gs.symCallExpression)
    {
        TSNode fn = parser::GetChildByField(unwrapped, gs.fieldFunction);
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
    TSNode typeNode = parser::GetChildByField(stmt, parser::GetGrammarSymbols().fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(stmt, parser::GetGrammarSymbols().fieldType);
    }
    const auto typeInfo = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode);
    const bool isHandle = typeInfo.isHandle;

    const uint32_t childCount = ts_node_named_child_count(stmt);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_named_child(stmt, i);
        if (ts_node_symbol(child) != parser::GetGrammarSymbols().symVariableDeclarator)
        {
            continue;
        }
        std::string name =
            NodeText(parser::GetChildByField(child, parser::GetGrammarSymbols().fieldName), ctx.sourceCode);
        TSNode val = parser::GetChildByField(child, parser::GetGrammarSymbols().fieldValue);
        if (ts_node_is_null(val))
        {
            val = parser::GetChildByField(child, parser::GetGrammarSymbols().fieldInit);
        }
        CheckNullExpression(val, state, ctx);

        if (isHandle && !name.empty())
        {
            state.vars[name] = EvaluateExpressionNullability({val, typeNode, name}, state, ctx);
        }
    }
}

void CheckAssignmentStmt(TSNode expr, FlowState& state, NullCheckContext& ctx)
{
    TSNode left = parser::GetChildByField(expr, parser::GetGrammarSymbols().fieldLeft);
    TSNode right = parser::GetChildByField(expr, parser::GetGrammarSymbols().fieldRight);
    CheckNullExpression(right, state, ctx);

    TSNode unwrappedLeft = UnwrapNullExpression(left);
    std::string name = GetIdentifierName(unwrappedLeft, ctx.sourceCode);
    if (!name.empty())
    {
        const Nullability assignedNullability = EvaluateExpressionNullability({right, {}, name}, state, ctx);
        if (assignedNullability != Nullability::Nullable || state.vars.contains(name))
        {
            state.vars[name] = assignedNullability;
        }
    }
    else
    {
        CheckNullExpression(left, state, ctx);
    }
}

} // namespace angel_lsp::analysis
