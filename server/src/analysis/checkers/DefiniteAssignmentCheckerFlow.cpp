#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "analysis/index/DefiniteAssignmentFacts.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace angel_lsp::analysis
{

ankerl::unordered_dense::set<std::string_view>
MergeBranchStates(const ankerl::unordered_dense::set<std::string_view>& a,
                  const ankerl::unordered_dense::set<std::string_view>& b)
{
    ankerl::unordered_dense::set<std::string_view> result = a;
    for (const auto& item : b)
    {
        result.insert(item);
    }
    return result;
}

void DefiniteAssignmentVisitor::Schedule(uint32_t recordIndex, DefiniteAssignmentState& state)
{
    if (recordIndex >= m_facts.records.size() || state.isTerminated || state.hasReturned)
    {
        return;
    }

    const auto& rec = m_facts.records[recordIndex];
    switch (rec.kind)
    {
    case DefiniteAssignmentStmtKind::Block:
    case DefiniteAssignmentStmtKind::If:
    case DefiniteAssignmentStmtKind::While:
    case DefiniteAssignmentStmtKind::For:
    case DefiniteAssignmentStmtKind::DoWhile:
    case DefiniteAssignmentStmtKind::Switch:
    case DefiniteAssignmentStmtKind::Case:
        m_work.push_back({recordIndex, state, {}, {}, 0, 0});
        break;
    default:
        StepLeaf(rec, state);
        break;
    }
}

void DefiniteAssignmentVisitor::MergeIfBranches(AssignmentWorkFrame& frame)
{
    auto& thenState = frame.branch;
    auto& elseState = frame.alternate;
    if (thenState.hasReturned && elseState.hasReturned)
    {
        frame.state.hasReturned = true;
    }
    else if (thenState.hasReturned)
    {
        frame.state = std::move(elseState);
    }
    else if (elseState.hasReturned)
    {
        frame.state = std::move(thenState);
    }
    else
    {
        frame.state.assignedVars = MergeBranchStates(thenState.assignedVars, elseState.assignedVars);
    }
}

bool DefiniteAssignmentVisitor::StepIf(AssignmentWorkFrame& frame)
{
    const auto& rec = m_facts.records[frame.recordIndex];
    const auto children = m_facts.ChildrenOf(rec);
    switch (frame.phase++)
    {
    case 0:
        EvaluateStatementExpressions(rec, frame.state);
        frame.branch = frame.state;
        if (!children.empty())
        {
            Schedule(children[0], frame.branch);
        }
        return false;
    case 1:
        frame.alternate = frame.state;
        if (children.size() > 1)
        {
            Schedule(children[1], frame.alternate);
        }
        return false;
    default:
        MergeIfBranches(frame);
        return true;
    }
}

bool DefiniteAssignmentVisitor::StepBlock(AssignmentWorkFrame& frame, bool clause)
{
    const auto& rec = m_facts.records[frame.recordIndex];
    const auto children = m_facts.ChildrenOf(rec);
    if (frame.nextChild >= children.size() || frame.state.hasReturned || frame.state.isTerminated)
    {
        return true;
    }

    const uint32_t childIndex = children[frame.nextChild++];
    if (clause && m_facts.records[childIndex].kind == DefiniteAssignmentStmtKind::Break)
    {
        return true;
    }

    Schedule(childIndex, frame.state);
    return false;
}

void DefiniteAssignmentVisitor::MergeLoopBody(AssignmentWorkFrame& frame, bool conditionAlwaysTrue)
{
    frame.state.assignedVars = MergeBranchStates(frame.state.assignedVars, frame.branch.assignedVars);
    if (conditionAlwaysTrue && !frame.branch.hasReturned && !frame.branch.isTerminated)
    {
        frame.state = std::move(frame.branch);
    }
}

bool DefiniteAssignmentVisitor::StepLoop(AssignmentWorkFrame& frame, bool isFor)
{
    const auto& rec = m_facts.records[frame.recordIndex];
    const auto children = m_facts.ChildrenOf(rec);
    switch (frame.phase++)
    {
    case 0:
        if (isFor && children.size() >= 2)
        {
            Schedule(children[0], frame.state);
        }
        return false;
    case 1:
        EvaluateStatementExpressions(rec, frame.state);
        frame.branch = frame.state;
        if (!children.empty())
        {
            Schedule(children.back(), frame.branch);
        }
        return false;
    default:
        MergeLoopBody(frame, rec.conditionAlwaysTrue);
        return true;
    }
}

bool DefiniteAssignmentVisitor::StepDoWhile(AssignmentWorkFrame& frame)
{
    const auto& rec = m_facts.records[frame.recordIndex];
    const auto children = m_facts.ChildrenOf(rec);
    if (frame.phase++ == 0)
    {
        if (!children.empty())
        {
            Schedule(children[0], frame.state);
        }
        return false;
    }
    EvaluateStatementExpressions(rec, frame.state);
    return true;
}

bool DefiniteAssignmentVisitor::StepSwitch(AssignmentWorkFrame& frame)
{
    const auto& rec = m_facts.records[frame.recordIndex];
    const auto children = m_facts.ChildrenOf(rec);
    if (frame.phase == 0)
    {
        EvaluateStatementExpressions(rec, frame.state);
        frame.alternate.hasReturned = true;
    }
    else if (!frame.branch.hasReturned)
    {
        frame.alternate.hasReturned = false;
        for (const auto& name : frame.branch.assignedVars)
        {
            frame.alternate.assignedVars.insert(name);
        }
    }

    if (frame.nextChild < children.size())
    {
        frame.phase = 1;
        const uint32_t clauseIndex = children[frame.nextChild++];
        frame.branch = frame.state;
        Schedule(clauseIndex, frame.branch);
        return false;
    }

    if (frame.phase != 0)
    {
        if (frame.alternate.hasReturned)
        {
            frame.state.hasReturned = true;
        }
        else
        {
            frame.state.assignedVars = std::move(frame.alternate.assignedVars);
        }
    }
    return true;
}

bool DefiniteAssignmentVisitor::Step(AssignmentWorkFrame& frame)
{
    const auto& rec = m_facts.records[frame.recordIndex];
    switch (rec.kind)
    {
    case DefiniteAssignmentStmtKind::Block:
        return StepBlock(frame, false);
    case DefiniteAssignmentStmtKind::Case:
        return StepBlock(frame, true);
    case DefiniteAssignmentStmtKind::If:
        return StepIf(frame);
    case DefiniteAssignmentStmtKind::While:
        return StepLoop(frame, false);
    case DefiniteAssignmentStmtKind::For:
        return StepLoop(frame, true);
    case DefiniteAssignmentStmtKind::DoWhile:
        return StepDoWhile(frame);
    case DefiniteAssignmentStmtKind::Switch:
        return StepSwitch(frame);
    default:
        StepLeaf(rec, frame.state);
        return true;
    }
}

void DefiniteAssignmentVisitor::StepLeaf(const DefiniteAssignmentStmtRecord& rec, DefiniteAssignmentState& state)
{
    switch (rec.kind)
    {
    case DefiniteAssignmentStmtKind::Expression:
        EvaluateStatementExpressions(rec, state);
        break;
    case DefiniteAssignmentStmtKind::VariableDeclaration:
    {
        const auto declarators = m_facts.DeclaratorsOf(rec);
        const auto reads = m_facts.ReadsOf(rec);
        for (const auto& decl : declarators)
        {
            if (decl.hasInitializer)
            {
                for (const auto& read : reads)
                {
                    if (read.bytes.startByte >= decl.bytes.startByte && read.bytes.endByte <= decl.bytes.endByte)
                    {
                        if (m_trackedLocals.contains(read.name) && !state.assignedVars.contains(read.name))
                        {
                            const uint64_t locationKey = (static_cast<uint64_t>(read.range.startLine) << 32) |
                                                         static_cast<uint64_t>(read.range.startCharacter);
                            if (m_reportedReads.insert(locationKey).second)
                            {
                                m_ctx.EmitAtRange(read.range, "as-warn-uninitialized-variable-read", read.name,
                                                  DiagnosticSeverity::Warning);
                            }
                        }
                    }
                }
                state.assignedVars.insert(decl.name);
            }
            else if (decl.isPrimitive)
            {
                m_trackedLocals.insert(decl.name);
            }
            else
            {
                state.assignedVars.insert(decl.name);
            }
        }
        break;
    }
    case DefiniteAssignmentStmtKind::Return:
        EvaluateStatementExpressions(rec, state);
        state.hasReturned = true;
        state.isTerminated = true;
        break;
    case DefiniteAssignmentStmtKind::Break:
    case DefiniteAssignmentStmtKind::Continue:
        state.isTerminated = true;
        break;
    default:
        break;
    }
}

} // namespace angel_lsp::analysis
