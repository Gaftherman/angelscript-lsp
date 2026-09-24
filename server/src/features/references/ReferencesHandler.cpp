#include "features/references/ReferencesHandler.h"
#include "analysis/TargetResolution.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::features
{
std::optional<ReferencesResult> GetReferences(const ReferencesRequest& request)
{
    TSNode node{};
    analysis::ResolveTargetRequest resolveReq{
        .uri = request.uri,
        .sourceCode = request.sourceCode,
        .tree = request.tree,
        .position = analysis::TargetPosition{request.position.line, request.position.character},
        .symbolTable = request.symbolTable,
        .scopeIndex = request.scopeIndex,
        .outNode = node,
        .logger = request.logger,
    };
    const auto target = analysis::ResolveTargetSymbol(resolveReq);

    if (!target.has_value() || ts_node_is_null(node))
    {
        return std::nullopt;
    }

    analysis::CollectOccurrencesRequest occReq{
        .target = *target,
        .currentUri = request.uri,
        .sourceCode = request.sourceCode,
        .tree = request.tree,
        .symbolTable = request.symbolTable,
        .scopeIndex = request.scopeIndex,
        .includeDeclaration = request.includeDeclaration,
        .logger = request.logger,
    };
    const auto occurrences = analysis::CollectOccurrences(occReq);

    if (occurrences.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::Location> results;
    results.reserve(occurrences.size());
    for (const auto& occ : occurrences)
    {
        results.push_back(lsp::Location{
            lsp::DocumentUri::parse(occ.fileUri),
            lsp::Range{
                lsp::Position{occ.range.startLine, occ.range.startCharacter},
                lsp::Position{occ.range.endLine, occ.range.endCharacter},
            },
        });
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
