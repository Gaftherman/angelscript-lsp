#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/index/ControlFlowFacts.h"

#include <span>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief `if (c);` and `else;` - a bare semicolon where the branch body belongs.
 *        AngelScript is stricter here than C++ and stricter than its own loops.
 */
static void CheckEmptyBranch(const ControlFlowRecord& record, DiagnosticContext& ctx)
{
    if (record.branch.hasEmptyConsequence)
    {
        ctx.EmitAtRange(record.branch.consequenceRange, "as-err-if-empty-statement");
    }
    if (record.branch.hasEmptyAlternative)
    {
        ctx.EmitAtRange(record.branch.alternativeRange, "as-err-else-empty-statement");
    }
}

bool ControlFlowVisitor::CheckLoopControl(const ControlFlowRecord& record, const ControlFlowState& state)
{
    if (record.kind == ControlFlowRecordKind::Break)
    {
        if (state.loopDepth == 0 && state.switchDepth == 0)
        {
            m_ctx.EmitAtRange(record.range, "as-err-break-outside-loop");
        }
        return true;
    }
    if (record.kind == ControlFlowRecordKind::Continue)
    {
        if (state.loopDepth == 0)
        {
            m_ctx.EmitAtRange(record.range, "as-err-continue-outside-loop");
        }
        return true;
    }
    return false;
}

void ControlFlowVisitor::UpdateBranchState(const ControlFlowRecord& record, ControlFlowState& state)
{
    if (record.kind == ControlFlowRecordKind::While || record.kind == ControlFlowRecordKind::For ||
        record.kind == ControlFlowRecordKind::Foreach || record.kind == ControlFlowRecordKind::DoWhile)
    {
        ++state.loopDepth;
    }
    else if (record.kind == ControlFlowRecordKind::Switch)
    {
        CheckSwitchLabels(record, m_facts, m_sourceCode, m_ctx);
        ++state.switchDepth;
    }
    else if (record.kind == ControlFlowRecordKind::If)
    {
        CheckEmptyBranch(record, m_ctx);
    }
}

} // namespace angel_lsp::analysis
