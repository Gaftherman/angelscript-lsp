#include "analysis/checkers/ControlFlowChecker.h"

#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "parser/GrammarNames.h"

#include <array>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

static void ReportFlowFindings(std::span<const IndexedFlowFinding> findings, DiagnosticContext& ctx)
{
    for (const auto& finding : findings)
    {
        switch (finding.kind)
        {
        case IndexedFlowFindingKind::Unreachable:
            ctx.EmitAtRange(finding.range, "as-warn-unreachable-code", DiagnosticSeverity::Warning);
            break;
        case IndexedFlowFindingKind::VariableInCase:
            ctx.EmitAtRange(finding.range, diagnostics::codes::VariableInSwitchCase);
            break;
        case IndexedFlowFindingKind::DefaultNotLast:
            ctx.EmitAtRange(finding.range, "as-err-default-must-be-last");
            break;
        }
    }
}

ControlFlowVisitor::ControlFlowVisitor(std::string_view sourceCode, DiagnosticContext& ctx, const NodeIndex& index)
    : m_sourceCode(sourceCode), m_ctx(ctx), m_index(index)
{
}

void ControlFlowVisitor::Run()
{
    const auto& grammar = parser::GetGrammarSymbols();
    ReportFlowFindings(m_index.FlowFindings(), m_ctx);
    const std::array nodes{grammar.symFuncDeclaration, grammar.symLambdaExpression, grammar.symIfStatement,
                           grammar.symSwitchStatement, grammar.symWhileStatement,   grammar.symDoWhileStatement,
                           grammar.symForStatement,    grammar.symForeachStatement, grammar.symReturnStatement,
                           grammar.symBreakStatement,  grammar.symContinueStatement};
    std::vector<std::pair<uint32_t, ControlFlowState>> frames;
    m_index.ForEachNodeOrdered(nodes, [&](TSNode node) { Visit(node, frames); });
}

void ControlFlowVisitor::Visit(TSNode node, std::vector<std::pair<uint32_t, ControlFlowState>>& frames)
{
    const uint32_t start = ts_node_start_byte(node);
    while (!frames.empty() && frames.back().first <= start)
    {
        frames.pop_back();
    }

    const ControlFlowState empty;
    const auto& enclosing = frames.empty() ? empty : frames.back().second;
    const auto& grammar = parser::GetGrammarSymbols();
    const TSSymbol symbol = ts_node_symbol(node);
    if (CheckLoopControl(node, symbol, enclosing))
    {
        return;
    }

    if (symbol == grammar.symReturnStatement)
    {
        CheckReturnStatement(node, enclosing);
    }
    else if (symbol == grammar.symFuncDeclaration || symbol == grammar.symLambdaExpression)
    {
        ControlFlowState state;
        CheckFunctionDeclaration(node, state);
        frames.emplace_back(ts_node_end_byte(node), std::move(state));
    }
    else
    {
        auto state = enclosing;
        UpdateBranchState(node, symbol, state);
        if (symbol != grammar.symIfStatement)
        {
            frames.emplace_back(ts_node_end_byte(node), std::move(state));
        }
    }
}

void CheckControlFlow(const ControlFlowCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    if (ctx.request.nodeIndex)
    {
        ControlFlowVisitor(request.sourceCode, ctx, *ctx.request.nodeIndex).Run();
    }
    else
    {
        ControlFlowVisitor(request.sourceCode, ctx, NodeIndex(request.root)).Run();
    }
}

} // namespace angel_lsp::analysis
