#include "analysis/checkers/null_safety/NullSafetyDecl.h"

#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/checkers/null_safety/NullSafetyExpr.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>

namespace angel_lsp::analysis
{
static bool IsTargetVariableTypeMatch(std::string_view targetVarName, std::string_view typeName, NullCheckContext& ctx)
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

static bool IsConstructorCall(std::string_view fnName, TSNode typeNode, std::string_view targetVarName,
                              NullCheckContext& ctx)
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
        const auto typeInfo = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode, &ctx.index);
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

static Nullability EvaluateExpressionNullability(const NullableEvalRequest& req, const FlowState& state,
                                                 NullCheckContext& ctx)
{
    if (ts_node_is_null(req.val))
    {
        return Nullability::DefinitelyNull;
    }
    TSNode unwrapped = UnwrapNullExpression(req.val, ctx.index);
    if (ts_node_is_null(unwrapped) || IsNullInitializer(unwrapped, &ctx.index))
    {
        return Nullability::DefinitelyNull;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(unwrapped);
    if (sym == gs.symConstructCallExpression || sym == gs.symThisExpression)
    {
        return Nullability::NonNull;
    }

    const std::string rhsVarName = GetIdentifierName(unwrapped, ctx);
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
        TSNode fn = SyntaxOperands(unwrapped, &ctx.index).Field(gs.fieldFunction);
        std::string fnName = GetIdentifierName(fn, ctx);
        if (fnName.empty() && !ts_node_is_null(fn))
        {
            fnName = parser::NodeText(fn, ctx.sourceCode);
        }
        if (IsConstructorCall(fnName, req.typeNode, req.targetVarName, ctx))
        {
            return Nullability::NonNull;
        }
    }

    return Nullability::Nullable;
}

void CheckVarDeclaration(TSNode stmt, FlowState& state, NullCheckContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands declaration(stmt, &ctx.index);
    TSNode typeNode = declaration.Field(gs.fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = declaration.Field(gs.fieldType);
    }
    const bool isHandle = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode, &ctx.index).isHandle;

    declaration.ForEachNamedOperands(
        [&](const SyntaxOperands& declarator)
        {
            if (declarator.Symbol() != gs.symVariableDeclarator)
            {
                return true;
            }
            const std::string name = parser::NodeText(declarator.Field(gs.fieldName), ctx.sourceCode);
            TSNode val = declarator.Field(gs.fieldValue);
            if (ts_node_is_null(val))
            {
                val = declarator.Field(gs.fieldInit);
            }
            CheckNullExpression(val, state, ctx);

            if (isHandle && !name.empty())
            {
                state.vars[name] = EvaluateExpressionNullability({val, typeNode, name}, state, ctx);
            }
            return true;
        });
}

void CheckAssignmentStmt(TSNode expr, FlowState& state, NullCheckContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands assignment(expr, &ctx.index);
    TSNode left = assignment.Field(gs.fieldLeft);
    TSNode right = assignment.Field(gs.fieldRight);
    CheckNullExpression(right, state, ctx);

    TSNode unwrappedLeft = UnwrapNullExpression(left, ctx.index);
    std::string name = GetIdentifierName(unwrappedLeft, ctx);
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
