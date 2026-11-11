#include "analysis/checkers/ControlFlowChecker.h"

#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ControlFlowFacts.h"
#include "analysis/index/NodeIndex.h"

#include <span>
#include <string_view>
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

ControlFlowVisitor::ControlFlowVisitor(std::string_view sourceCode, DiagnosticContext& ctx,
                                       const ControlFlowFacts& facts)
    : m_sourceCode(sourceCode), m_ctx(ctx), m_facts(facts)
{
}

void ControlFlowVisitor::Run()
{
    std::vector<std::pair<uint32_t, ControlFlowState>> frames;
    for (const auto& record : m_facts.records)
    {
        switch (record.kind)
        {
        case ControlFlowRecordKind::Function:
        case ControlFlowRecordKind::Lambda:
        case ControlFlowRecordKind::If:
        case ControlFlowRecordKind::Switch:
        case ControlFlowRecordKind::While:
        case ControlFlowRecordKind::DoWhile:
        case ControlFlowRecordKind::For:
        case ControlFlowRecordKind::Foreach:
        case ControlFlowRecordKind::Return:
        case ControlFlowRecordKind::Break:
        case ControlFlowRecordKind::Continue:
            Visit(record, frames);
            break;
        default:
            break;
        }
    }
}

void ControlFlowVisitor::Visit(const ControlFlowRecord& record,
                               std::vector<std::pair<uint32_t, ControlFlowState>>& frames)
{
    const uint32_t start = record.bytes.startByte;
    while (!frames.empty() && frames.back().first <= start)
    {
        frames.pop_back();
    }

    const ControlFlowState empty;
    const auto& enclosing = frames.empty() ? empty : frames.back().second;
    if (CheckLoopControl(record, enclosing))
    {
        return;
    }

    if (record.kind == ControlFlowRecordKind::Return)
    {
        CheckReturnStatement(record, enclosing);
    }
    else if (record.kind == ControlFlowRecordKind::Function || record.kind == ControlFlowRecordKind::Lambda)
    {
        ControlFlowState state;
        CheckFunctionDeclaration(record, state);
        frames.emplace_back(record.bytes.endByte, std::move(state));
    }
    else
    {
        auto state = enclosing;
        UpdateBranchState(record, state);
        if (record.kind != ControlFlowRecordKind::If)
        {
            frames.emplace_back(record.bytes.endByte, std::move(state));
        }
    }
}

void CheckControlFlow(const ControlFlowFacts& facts, std::string_view sourceCode, DiagnosticContext& ctx)
{
    if (ctx.request.nodeIndex)
    {
        ReportFlowFindings(ctx.request.nodeIndex->FlowFindings(), ctx);
    }
    ControlFlowVisitor visitor(sourceCode, ctx, facts);
    visitor.Run();
}

void CheckControlFlow(const ControlFlowFacts& facts, DiagnosticContext& ctx)
{
    CheckControlFlow(facts, ctx.request.sourceCode, ctx);
}

void CheckControlFlow(const ControlFlowCheckRequest& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty())
    {
        return;
    }

    if (request.facts)
    {
        CheckControlFlow(*request.facts, request.sourceCode, ctx);
        return;
    }

    const NodeIndex* index = request.nodeIndex ? request.nodeIndex : ctx.request.nodeIndex;
    if (index)
    {
        const ControlFlowFacts facts = BuildControlFlowFacts(*index, request.sourceCode);
        CheckControlFlow(facts, request.sourceCode, ctx);
    }
}

} // namespace angel_lsp::analysis
