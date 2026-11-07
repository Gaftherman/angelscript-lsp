#include "analysis/index/BreakpointFacts.h"

#include "analysis/index/BreakpointFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <array>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsNodeInvalid(TSNode node) noexcept
{
    return ts_node_is_null(node) || ts_node_is_missing(node) || ts_node_is_error(node) ||
           ts_node_start_byte(node) >= ts_node_end_byte(node);
}

std::vector<SourceRange> CollectBreakpointSpans(const NodeIndex& index, uint32_t startLine, uint32_t endLine)
{
    std::vector<SourceRange> results;
    if (startLine > endLine)
    {
        return results;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 14> targetSymbols = {
        gs.symFuncDeclaration,     gs.symAccessor,
        gs.symLambdaExpression,    gs.symDoWhileStatement,
        gs.symIfStatement,         gs.symWhileStatement,
        gs.symForeachStatement,    gs.symSwitchStatement,
        gs.symForStatement,        gs.symVariableDeclaration,
        gs.symExpressionStatement, gs.symReturnStatement,
        gs.symBreakStatement,      gs.symContinueStatement,
    };

    BreakpointSpanContext ctx{gs, startLine, endLine, results, false};

    index.ForEachNodeOrdered(targetSymbols,
                             [&](TSNode current)
                             {
                                 if (IsNodeInvalid(current))
                                 {
                                     return;
                                 }
                                 const TSPoint start = ts_node_start_point(current);
                                 const TSPoint end = ts_node_end_point(current);
                                 if (start.row > endLine && end.row > endLine)
                                 {
                                     return;
                                 }
                                 if (start.row < startLine && end.row < startLine)
                                 {
                                     return;
                                 }

                                 const TSSymbol sym = ts_node_symbol(current);
                                 ProcessBreakpointStatement(current, sym, ctx);
                             });

    return results;
}

} // namespace angel_lsp::analysis
