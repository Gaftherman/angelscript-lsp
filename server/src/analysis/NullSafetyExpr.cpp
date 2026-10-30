#include "analysis/NullSafetyExpr.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/SymbolTable.h"
#include "analysis/TypeExtraction.h"
#include "parser/GrammarNames.h"
#include <memory>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
void ApplyAssertions(FlowState& state, const std::vector<NullAssertion>& assertions)
{
    for (const auto& a : assertions)
    {
        state.vars[a.varName] = a.state;
    }
}

void CheckMemberDereference(TSNode obj, FlowState& state, NullCheckContext& ctx)
{
    if (ts_node_is_null(obj))
    {
        return;
    }
    std::string name = GetIdentifierName(obj, ctx.sourceCode);
    if (!name.empty())
    {
        auto it = state.vars.find(name);
        if (it != state.vars.end() &&
            (it->second == Nullability::Nullable || it->second == Nullability::DefinitelyNull))
        {
            const bool reportAll =
                ctx.diagCtx.request.diagnostics && ctx.diagCtx.request.diagnostics->reportAllNullDereferences;
            if (reportAll || !state.warnedVars.contains(name))
            {
                ctx.diagCtx.EmitAtRange(ToSourceRange(obj), diagnostics::codes::PossibleNullDereference, name,
                                        DiagnosticSeverity::Warning);
                state.warnedVars.insert(name);
            }
            if (!reportAll)
            {
                it->second = Nullability::NonNull;
            }
        }
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol objSym = ts_node_symbol(obj);
    if (objSym == gs.symCastExpression)
    {
        TSNode typeNode = parser::GetChildByField(obj, gs.fieldType);
        const auto typeInfo = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode);
        if (typeInfo.isHandle)
        {
            std::string typeText = NodeText(typeNode, ctx.sourceCode);
            ctx.diagCtx.EmitAtRange(ToSourceRange(obj), diagnostics::codes::PossibleNullDereference, typeText,
                                    DiagnosticSeverity::Warning);
        }
    }
}

struct NullWorkItem
{
    TSNode expr;
    std::shared_ptr<FlowState> state;
};

void ProcessBinaryExpr(TSNode expr, const parser::GrammarSymbols& gs, std::vector<NullWorkItem>& worklist,
                       NullCheckContext& ctx, FlowState& state, const std::shared_ptr<FlowState>& statePtr)
{
    TSNode left = parser::GetChildByField(expr, gs.fieldLeft);
    TSNode opNode = parser::GetChildByField(expr, gs.fieldOperator);
    TSNode right = parser::GetChildByField(expr, gs.fieldRight);
    const TSSymbol opSym = ts_node_symbol(opNode);

    if (opSym == gs.tokAmpAmp || opSym == gs.tokAnd)
    {
        std::vector<NullAssertion> posAssertions;
        std::vector<NullAssertion> negAssertions;
        ExtractConditionAssertions(left, ctx.sourceCode, posAssertions, negAssertions);
        auto rightState = std::make_shared<FlowState>(state);
        ApplyAssertions(*rightState, posAssertions);

        worklist.push_back({right, rightState});
        worklist.push_back({left, statePtr});
    }
    else if (opSym == gs.tokPipePipe || opSym == gs.tokOr)
    {
        std::vector<NullAssertion> posAssertions;
        std::vector<NullAssertion> negAssertions;
        ExtractConditionAssertions(left, ctx.sourceCode, posAssertions, negAssertions);
        auto rightState = std::make_shared<FlowState>(state);
        ApplyAssertions(*rightState, negAssertions);

        worklist.push_back({right, rightState});
        worklist.push_back({left, statePtr});
    }
    else
    {
        worklist.push_back({right, statePtr});
        worklist.push_back({left, statePtr});
    }
}

void ProcessCallExpr(TSNode expr, const parser::GrammarSymbols& gs, std::vector<NullWorkItem>& worklist,
                     const std::shared_ptr<FlowState>& statePtr)
{
    TSNode argsNode = parser::GetChildByField(expr, gs.fieldArguments);
    const uint32_t count = ts_node_is_null(argsNode) ? 0 : ts_node_named_child_count(argsNode);
    for (uint32_t i = count; i > 0; --i)
    {
        worklist.push_back({ts_node_named_child(argsNode, i - 1), statePtr});
    }
    worklist.push_back({parser::GetChildByField(expr, gs.fieldFunction), statePtr});
}

void DispatchExprWorkItem(const NullWorkItem& item, const parser::GrammarSymbols& gs,
                          std::vector<NullWorkItem>& worklist, NullCheckContext& ctx)
{
    TSNode expr = item.expr;
    FlowState& state = *item.state;
    const TSSymbol exprSym = ts_node_symbol(expr);

    if (exprSym == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(expr, gs.fieldObject);
        CheckMemberDereference(obj, state, ctx);
        worklist.push_back({obj, item.state});
    }
    else if (exprSym == gs.symIndexExpression)
    {
        TSNode obj = parser::GetChildByField(expr, gs.fieldObject);
        CheckMemberDereference(obj, state, ctx);
        worklist.push_back({parser::GetChildByField(expr, gs.fieldIndex), item.state});
        worklist.push_back({obj, item.state});
    }
    else if (exprSym == gs.symCallExpression)
    {
        ProcessCallExpr(expr, gs, worklist, item.state);
    }
    else if (exprSym == gs.symBinaryExpression)
    {
        ProcessBinaryExpr(expr, gs, worklist, ctx, state, item.state);
    }
    else if (exprSym == gs.symCastExpression)
    {
        worklist.push_back({parser::GetChildByField(expr, gs.fieldValue), item.state});
    }
    else if (exprSym == gs.symParenthesizedExpression)
    {
        worklist.push_back({ts_node_named_child(expr, 0), item.state});
    }
    else if (exprSym == gs.symTernaryExpression)
    {
        worklist.push_back({parser::GetChildByField(expr, gs.fieldAlternative), item.state});
        worklist.push_back({parser::GetChildByField(expr, gs.fieldConsequence), item.state});
        worklist.push_back({parser::GetChildByField(expr, gs.fieldCondition), item.state});
    }
}
} // namespace

void CheckNullExpression(TSNode expr, FlowState& state, NullCheckContext& ctx)
{
    if (ts_node_is_null(expr))
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    auto sharedRootState = std::make_shared<FlowState>(state);

    std::vector<NullWorkItem> worklist;
    worklist.push_back({expr, sharedRootState});

    while (!worklist.empty())
    {
        NullWorkItem item = std::move(worklist.back());
        worklist.pop_back();

        if (ts_node_is_null(item.expr))
        {
            continue;
        }
        DispatchExprWorkItem(item, gs, worklist, ctx);
    }

    state = *sharedRootState;
}

} // namespace angel_lsp::analysis
