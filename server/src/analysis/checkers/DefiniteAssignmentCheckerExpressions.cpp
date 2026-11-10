#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "analysis/index/DefiniteAssignmentFacts.h"

#include <cstdint>
#include <string_view>

namespace angel_lsp::analysis
{

void DefiniteAssignmentVisitor::EvaluateStatementExpressions(const DefiniteAssignmentStmtRecord& rec,
                                                             DefiniteAssignmentState& state)
{
    const auto calls = m_facts.CallsOf(rec);
    for (const auto& call : calls)
    {
        ProcessCall(call, state);
    }

    const auto reads = m_facts.ReadsOf(rec);
    for (const auto& read : reads)
    {
        if (m_trackedLocals.contains(read.name) && !state.assignedVars.contains(read.name))
        {
            const uint64_t locationKey =
                (static_cast<uint64_t>(read.range.startLine) << 32) | static_cast<uint64_t>(read.range.startCharacter);
            if (m_reportedReads.insert(locationKey).second)
            {
                m_ctx.EmitAtRange(read.range, "as-warn-uninitialized-variable-read", read.name,
                                  DiagnosticSeverity::Warning);
            }
        }
    }

    const auto assigns = m_facts.AssignmentsOf(rec);
    for (const auto& assign : assigns)
    {
        if (assign.opIsAssign && !assign.left.foldedName.empty())
        {
            if (m_trackedLocals.contains(assign.left.foldedName))
            {
                state.assignedVars.insert(assign.left.foldedName);
            }
        }
    }
}

} // namespace angel_lsp::analysis
