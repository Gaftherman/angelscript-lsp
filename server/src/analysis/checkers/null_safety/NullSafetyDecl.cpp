#include "analysis/checkers/null_safety/NullSafetyDecl.h"

#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/checkers/null_safety/NullSafetyExpr.h"
#include "analysis/index/SymbolTable.h"
#include "utils/ScopedNameUtils.h"

#include <string_view>

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

static bool IsConstructorCall(std::string_view fnName, std::string_view typeBaseName, std::string_view targetVarName,
                              NullCheckContext& ctx)
{
    if (fnName.empty())
    {
        return false;
    }
    const std::string_view shortName = utils::LastScopeSegment(fnName);
    if (shortName.empty())
    {
        return false;
    }

    if (!typeBaseName.empty() && shortName == typeBaseName)
    {
        return true;
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
    if (req.val == NoNullFlowNode)
    {
        return Nullability::DefinitelyNull;
    }
    const NullFlowNodeId unwrapped = UnwrapNullExpression(req.val, ctx.facts);
    const auto* unwrappedNode = ctx.facts.Find(unwrapped);
    if (!unwrappedNode || unwrappedNode->isNullInitializer)
    {
        return Nullability::DefinitelyNull;
    }

    if (unwrappedNode->kind == NullFlowNodeKind::ConstructCall || unwrappedNode->kind == NullFlowNodeKind::This)
    {
        return Nullability::NonNull;
    }

    const std::string_view rhsVarName = GetIdentifierName(unwrapped, ctx);
    if (!rhsVarName.empty())
    {
        auto it = state.vars.find(rhsVarName);
        if (it != state.vars.end())
        {
            return it->second;
        }
    }

    if (unwrappedNode->kind == NullFlowNodeKind::Call)
    {
        const NullFlowNodeId fn = unwrappedNode->first;
        std::string_view fnName = GetIdentifierName(fn, ctx);
        if (fnName.empty() && fn != NoNullFlowNode)
        {
            if (const auto* fnNode = ctx.facts.Find(fn))
            {
                fnName = fnNode->text;
            }
        }
        if (IsConstructorCall(fnName, req.typeBaseName, req.targetVarName, ctx))
        {
            return Nullability::NonNull;
        }
    }

    return Nullability::Nullable;
}

/**
 * @brief Checks a variable declaration statement for handle variables and initialization.
 * @param[in] stmt Variable declaration AST node id.
 * @param[in,out] state Flow state updated with declared variables and their nullability.
 * @param[in,out] ctx Null check context for source code and diagnostic emission.
 */
void CheckVarDeclaration(NullFlowNodeId stmt, FlowState& state, NullCheckContext& ctx)
{
    const auto* node = ctx.facts.Find(stmt);
    if (!node)
    {
        return;
    }
    const bool isHandle = node->typeIsHandle;
    const std::string_view typeBaseName = node->typeNameIndex < ctx.facts.typeBaseNames.size()
                                              ? std::string_view(ctx.facts.typeBaseNames[node->typeNameIndex])
                                              : std::string_view{};

    const uint32_t end = node->declaratorBegin + node->declaratorCount;
    for (uint32_t i = node->declaratorBegin; i < end && i < ctx.facts.declarators.size(); ++i)
    {
        const auto& d = ctx.facts.declarators[i];
        const std::string_view name = d.name;
        const NullFlowNodeId val = d.value;
        CheckNullExpression(val, state, ctx);

        if (isHandle && !name.empty())
        {
            state.vars[name] = EvaluateExpressionNullability({val, typeBaseName, name}, state, ctx);
        }
    }
}

/**
 * @brief Checks an assignment expression for handle reassignment.
 * @param[in] expr Assignment expression AST node id.
 * @param[in,out] state Flow state updated with reassigned nullability.
 * @param[in,out] ctx Null check context for source code and diagnostic emission.
 */
void CheckAssignmentStmt(NullFlowNodeId expr, FlowState& state, NullCheckContext& ctx)
{
    const auto* node = ctx.facts.Find(expr);
    if (!node)
    {
        return;
    }
    const NullFlowNodeId left = node->first;
    const NullFlowNodeId right = node->second;
    CheckNullExpression(right, state, ctx);

    const NullFlowNodeId unwrappedLeft = UnwrapNullExpression(left, ctx.facts);
    const std::string_view name = GetIdentifierName(unwrappedLeft, ctx);
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
