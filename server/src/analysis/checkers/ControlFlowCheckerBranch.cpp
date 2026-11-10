#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief `if (c);` and `else;` - a bare semicolon where the branch body belongs.
 *        AngelScript is stricter here than C++ and stricter than its own loops, which is the whole
 */
static void CheckEmptyBranch(TSNode node, DiagnosticContext& ctx)
{
    if (ts_node_has_error(node))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSNode consequence = parser::GetChildByField(node, gs.fieldConsequence);
    if (!ts_node_is_null(consequence) && ts_node_symbol(consequence) == gs.tokSemicolon)
    {
        const TSPoint start = ts_node_start_point(consequence);
        const TSPoint end = ts_node_end_point(consequence);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-if-empty-statement");
    }

    const TSNode alternative = parser::GetChildByField(node, gs.fieldAlternative);
    if (!ts_node_is_null(alternative) && ts_node_symbol(alternative) == gs.tokSemicolon)
    {
        const TSPoint start = ts_node_start_point(alternative);
        const TSPoint end = ts_node_end_point(alternative);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-else-empty-statement");
    }
}

bool ControlFlowVisitor::CheckLoopControl(TSNode node, TSSymbol sym, const ControlFlowState& state)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symBreakStatement)
    {
        if (state.loopDepth == 0 && state.switchDepth == 0)
        {
            m_ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-break-outside-loop");
        }
        return true;
    }
    if (sym == gs.symContinueStatement)
    {
        if (state.loopDepth == 0)
        {
            m_ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-continue-outside-loop");
        }
        return true;
    }
    return false;
}

void ControlFlowVisitor::UpdateBranchState(TSNode node, TSSymbol sym, ControlFlowState& state)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symWhileStatement || sym == gs.symForStatement || sym == gs.symForeachStatement ||
        sym == gs.symDoWhileStatement)
    {
        ++state.loopDepth;
    }
    else if (sym == gs.symSwitchStatement)
    {
        CheckSwitchLabels(m_index.SwitchLabels(ts_node_start_byte(node)), m_sourceCode, m_ctx);
        ++state.switchDepth;
    }
    else if (sym == gs.symIfStatement)
    {
        CheckEmptyBranch(node, m_ctx);
    }
}

} // namespace angel_lsp::analysis
