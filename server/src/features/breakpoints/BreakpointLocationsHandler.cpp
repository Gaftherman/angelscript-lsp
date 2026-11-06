#include "BreakpointLocationsHandler.h"

#include "features/breakpoints/BreakpointStatementClassifier.h"
#include "parser/ASTUtils.h"

#include <algorithm>
#include <array>

namespace angel_lsp::features
{

std::vector<BreakpointLocationItem> BreakpointLocationsHandler::Collect(const BreakpointLocationsRequest& request)
{
    std::vector<BreakpointLocationItem> results;
    if (!request.nodeIndex || request.startLine > request.endLine)
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

    CollectorContext ctx{request, gs, false, results};

    request.nodeIndex->ForEachNodeOrdered(targetSymbols,
                                          [&](TSNode current)
                                          {
                                              if (IsNodeInvalid(current))
                                              {
                                                  return;
                                              }
                                              const TSPoint start = ts_node_start_point(current);
                                              const TSPoint end = ts_node_end_point(current);
                                              if (start.row > request.endLine && end.row > request.endLine)
                                              {
                                                  return;
                                              }
                                              if (start.row < request.startLine && end.row < request.startLine)
                                              {
                                                  return;
                                              }

                                              const TSSymbol sym = ts_node_symbol(current);
                                              HandleFunctionLike(current, sym, ctx);

                                              if (HandleLoopsAndBranches(current, sym, ctx))
                                              {
                                                  return;
                                              }

                                              if (sym == gs.symVariableDeclaration)
                                              {
                                                  ctx.inStatementBlock = !ts_node_is_null(
                                                      parser::FindAncestorWithSymbol(current, gs.symStatementBlock));
                                              }
                                              HandleLeafStatement(current, sym, ctx);
                                          });

    std::sort(results.begin(), results.end(),
              [](const BreakpointLocationItem& a, const BreakpointLocationItem& b) noexcept
              {
                  if (a.line != b.line)
                      return a.line < b.line;
                  if (a.column != b.column)
                      return a.column < b.column;
                  if (a.endLine != b.endLine)
                      return a.endLine < b.endLine;
                  return a.endColumn < b.endColumn;
              });

    auto isDuplicate = [](const BreakpointLocationItem& a, const BreakpointLocationItem& b) noexcept
    { return a.line == b.line && a.column == b.column && a.endLine == b.endLine && a.endColumn == b.endColumn; };
    results.erase(std::unique(results.begin(), results.end(), isDuplicate), results.end());

    return results;
}

} // namespace angel_lsp::features
