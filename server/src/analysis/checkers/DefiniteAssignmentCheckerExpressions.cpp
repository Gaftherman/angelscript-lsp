#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "analysis/index/DefiniteAssignmentFacts.h"

#include <cstdint>
#include <string_view>

namespace angel_lsp::analysis
{

void DefiniteAssignmentVisitor::CheckRead(std::string_view name, const SourceRange& range,
                                          const DefiniteAssignmentState& state)
{
    if (!m_trackedLocals.contains(name) || state.assignedVars.contains(name))
    {
        return;
    }
    const uint64_t locationKey =
        (static_cast<uint64_t>(range.startLine) << 32) | static_cast<uint64_t>(range.startCharacter);
    if (m_reportedReads.insert(locationKey).second)
    {
        m_ctx.EmitAtRange(range, "as-warn-uninitialized-variable-read", name, DiagnosticSeverity::Warning);
    }
}

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
        CheckRead(read.name, read.range, state);
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
