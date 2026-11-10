#include "analysis/checkers/AccessChecker.h"

#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "parser/GrammarNames.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"

#include <array>
#include <span>

namespace angel_lsp::analysis
{

static void CheckMemberAccessIndexed(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode thisNode : request.nodeIndex->Nodes(gs.symThisExpression))
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }
        const TSPoint start = ts_node_start_point(thisNode);
        CheckClosureDisallowedAccess(thisNode, FindInnermostScope(request.scopeRoot, start.row, start.column), "this",
                                     ctx);
    }

    const std::array<TSSymbol, 3> kSymbols = {gs.symMemberExpression, gs.symScopedIdentifier, gs.symIdentifier};
    request.nodeIndex->ForEachNodeOrdered(std::span<const TSSymbol>(kSymbols.data(), kSymbols.size()),
                                          [&](TSNode node)
                                          {
                                              if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
                                              {
                                                  return;
                                              }
                                              if (ts_node_symbol(node) == gs.symMemberExpression)
                                              {
                                                  const TSPoint pt = ts_node_start_point(node);
                                                  const Scope* scope =
                                                      request.mediator
                                                          ? request.mediator->ScopeAtPoint(pt.row, pt.column)
                                                          : FindInnermostScope(request.scopeRoot, pt.row, pt.column);
                                                  CheckMemberExpression(node, request, scope, ctx);
                                              }
                                              else
                                              {
                                                  CheckIdentifierNode(node, request, ctx);
                                              }
                                          });
}

/**
 * @brief Check member access.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckMemberAccess(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.rootNode) || request.sourceCode.empty())
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        CheckMemberAccessIndexed(request, ctx);
        return;
    }

    NodeIndex localIndex(request.rootNode);
    AstCheckContext indexedRequest = request;
    indexedRequest.nodeIndex = &localIndex;
    CheckMemberAccessIndexed(indexedRequest, ctx);
}

} // namespace angel_lsp::analysis
