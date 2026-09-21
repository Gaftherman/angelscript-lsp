#include "features/references/ReferencesHandler.h"
#include "features/symbol_resolution/SymbolResolution.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::features
{
// This file used to carry its own copy of the resolution and collection code - the same ~780
// lines rename had, differing only in whitespace, comments, and `sourceCode` against
// `request.sourceCode`. They agreed, and they agreed by nobody having edited one without the
// other. See features/symbol_resolution/SymbolResolution.h.
using namespace angel_lsp::features::resolution;

std::optional<ReferencesResult> GetReferences(const ReferencesRequest& request)
{
    TSNode node{};
    ResolveTargetRequest resolveReq{
        .uri = request.uri,
        .sourceCode = request.sourceCode,
        .tree = request.tree,
        .position = request.position,
        .symbolTable = request.symbolTable,
        .scopeIndex = request.scopeIndex,
        .outNode = node,
        .logger = request.logger,
    };
    const auto target = ResolveTargetSymbol(resolveReq);

    if (!target.has_value() || ts_node_is_null(node))
    {
        return std::nullopt;
    }

    CollectOccurrencesRequest occReq{
        .target = *target,
        .currentUri = request.uri,
        .sourceCode = request.sourceCode,
        .tree = request.tree,
        .symbolTable = request.symbolTable,
        .scopeIndex = request.scopeIndex,
        .includeDeclaration = request.includeDeclaration,
        .logger = request.logger,
    };
    auto results = CollectOccurrences(occReq);

    if (results.empty())
    {
        return std::nullopt;
    }

    // Sorted so the answer is stable across runs: CollectOccurrences walks documents in
    // whatever order the symbol table hands them over, which is a hash order.
    std::sort(results.begin(), results.end(),
              [](const lsp::Location& a, const lsp::Location& b)
              {
                  if (a.uri.toString() != b.uri.toString())
                  {
                      return a.uri.toString() < b.uri.toString();
                  }
                  if (a.range.start.line != b.range.start.line)
                  {
                      return a.range.start.line < b.range.start.line;
                  }
                  return a.range.start.character < b.range.start.character;
              });

    return results;
}
} // namespace angel_lsp::features
