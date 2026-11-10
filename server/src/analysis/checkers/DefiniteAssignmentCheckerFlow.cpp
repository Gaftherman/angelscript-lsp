#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"

#include <ankerl/unordered_dense.h>
#include <string_view>
#include <tree_sitter/api.h>
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

void DefiniteAssignmentVisitor::Schedule(const IndexedSyntaxNode* operand, DefiniteAssignmentState& state)
{
    if (!operand || state.isTerminated || state.hasReturned)
    {
        return;
    }
    if (operand->statement)
    {
        m_work.push_back({operand, state, {}, {}, First(operand)});
    }
    else
    {
        CheckExpressionReads(operand, state);
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
    const auto& gs = parser::GetGrammarSymbols();
    switch (frame.phase++)
    {
    case 0:
        CheckExpressionReads(FindFieldOrNamed(frame.node, gs.fieldCondition), frame.state);
        frame.branch = frame.state;
        Schedule(FindField(frame.node, gs.fieldConsequence), frame.branch);
        return false;
    case 1:
        frame.alternate = frame.state;
        Schedule(FindField(frame.node, gs.fieldAlternative), frame.alternate);
        return false;
    default:
        MergeIfBranches(frame);
        return true;
    }
}

bool DefiniteAssignmentVisitor::StepBlock(AssignmentWorkFrame& frame, bool clause)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (frame.phase == 0 && clause && !frame.node->defaultClause)
    {
        frame.next = Next(NthNamed(frame.next, 0));
    }
    frame.phase = 1;
    const IndexedSyntaxNode* operand = NthNamed(frame.next, 0);
    if (!operand || frame.state.hasReturned || frame.state.isTerminated ||
        (clause && operand->symbol == gs.symBreakStatement))
    {
        return true;
    }
    frame.next = Next(operand);
    Schedule(operand, frame.state);
    return false;
}

void DefiniteAssignmentVisitor::MergeLoopBody(AssignmentWorkFrame& frame, TSNode condition, bool isFor)
{
    frame.state.assignedVars = MergeBranchStates(frame.state.assignedVars, frame.branch.assignedVars);
    const std::string_view raw = parser::NodeText(condition, m_request.sourceCode);
    const std::string_view value = isFor ? utils::TrimView(raw) : raw;
    if ((value == "true" || (isFor && (value.empty() || value == ";"))) && !frame.branch.hasReturned &&
        !frame.branch.isTerminated)
    {
        frame.state = std::move(frame.branch);
    }
}

bool DefiniteAssignmentVisitor::StepLoop(AssignmentWorkFrame& frame, TSSymbol symbol)
{
    const auto& gs = parser::GetGrammarSymbols();
    const bool isFor = symbol == gs.symForStatement;
    switch (frame.phase++)
    {
    case 0:
        if (isFor)
        {
            Schedule(FindField(frame.node, gs.fieldInit), frame.state);
        }
        return false;
    case 1:
        frame.next = isFor ? FindField(frame.node, gs.fieldCondition) : FindFieldOrNamed(frame.node, gs.fieldCondition);
        CheckExpressionReads(frame.next, frame.state);
        frame.branch = frame.state;
        Schedule(FindField(frame.node, gs.fieldBody), frame.branch);
        return false;
    default:
        if (isFor)
        {
            CheckExpressionReads(FindField(frame.node, gs.fieldUpdate), frame.branch);
        }
        MergeLoopBody(frame, Syntax(frame.next), isFor);
        return true;
    }
}

bool DefiniteAssignmentVisitor::StepDoWhile(AssignmentWorkFrame& frame)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (frame.phase++ == 0)
    {
        Schedule(FindField(frame.node, gs.fieldBody), frame.state);
        return false;
    }
    CheckExpressionReads(FindFieldOrNamed(frame.node, gs.fieldCondition, 1), frame.state);
    return true;
}

bool DefiniteAssignmentVisitor::StepSwitch(AssignmentWorkFrame& frame)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (frame.phase == 0)
    {
        CheckExpressionReads(FindFieldOrNamed(frame.node, gs.fieldCondition), frame.state);
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

    while (frame.next && frame.next->symbol != gs.symCaseClause)
    {
        frame.next = Next(frame.next);
    }
    if (const IndexedSyntaxNode* clause = frame.next)
    {
        frame.phase = 1;
        frame.next = Next(clause);
        frame.branch = frame.state;
        Schedule(clause, frame.branch);
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
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol symbol = frame.node->symbol;
    if (symbol == gs.symStatementBlock || symbol == gs.symCaseClause)
    {
        return StepBlock(frame, symbol == gs.symCaseClause);
    }
    if (symbol == gs.symIfStatement)
    {
        return StepIf(frame);
    }
    if (symbol == gs.symWhileStatement || symbol == gs.symForStatement)
    {
        return StepLoop(frame, symbol);
    }
    if (symbol == gs.symDoWhileStatement)
    {
        return StepDoWhile(frame);
    }
    if (symbol == gs.symSwitchStatement)
    {
        return StepSwitch(frame);
    }
    StepLeaf(frame, symbol);
    return true;
}

void DefiniteAssignmentVisitor::StepLeaf(AssignmentWorkFrame& frame, TSSymbol symbol)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (symbol == gs.symExpressionStatement)
    {
        CheckExpressionReads(NthNamed(frame.next, 0), frame.state);
    }
    else if (symbol == gs.symVariableDeclaration)
    {
        AnalyzeVariableDeclaration(frame.node, frame.state);
    }
    else if (symbol == gs.symReturnStatement)
    {
        CheckExpressionReads(FindFieldOrNamed(frame.node, gs.fieldValue), frame.state);
        frame.state.hasReturned = true;
        frame.state.isTerminated = true;
    }
    else if (symbol == gs.symBreakStatement || symbol == gs.symContinueStatement)
    {
        frame.state.isTerminated = true;
    }
}

} // namespace angel_lsp::analysis
