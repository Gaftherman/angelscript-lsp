#include "analysis/checkers/ConstChecker.h"

#include "analysis/checkers/ConstCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "parser/GrammarNames.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"

#include <array>
#include <span>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Iterates over indexed assignment and call expressions checking for const violations.
 * @param[in] request Contextual request bundle containing pre-indexed syntax tree.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void CheckConstCorrectnessIndexed(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> kSymbols = {gs.symAssignmentExpression, gs.symCallExpression};
    request.nodeIndex->ForEachNodeOrdered(std::span<const TSSymbol>(kSymbols.data(), kSymbols.size()),
                                          [&](TSNode node)
                                          {
                                              if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
                                              {
                                                  return;
                                              }
                                              const TSPoint pt = ts_node_start_point(node);
                                              const Scope* scope =
                                                  request.mediator
                                                      ? request.mediator->ScopeAtPoint(pt.row, pt.column)
                                                      : FindInnermostScope(request.scopeRoot, pt.row, pt.column);
                                              if (ts_node_symbol(node) == gs.symAssignmentExpression)
                                              {
                                                  CheckAssignment(node, request, scope, ctx);
                                              }
                                              else
                                              {
                                                  CheckMethodCall(node, request, scope, ctx);
                                              }
                                          });
}

/**
 * @brief Check const correctness.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckConstCorrectness(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.rootNode) || request.sourceCode.empty())
    {
        return;
    }

    // A stub describes an API rather than using one, so it has no expressions worth judging -
    // the same exemption every other use-site pass carries.
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        CheckConstCorrectnessIndexed(request, ctx);
        return;
    }

    NodeIndex localIndex(request.rootNode);
    AstCheckContext indexedRequest = request;
    indexedRequest.nodeIndex = &localIndex;
    CheckConstCorrectnessIndexed(indexedRequest, ctx);
}

} // namespace angel_lsp::analysis
