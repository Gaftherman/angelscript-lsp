#include "features/references/ReferencesHandler.h"

#include "analysis/resolution/TargetResolution.h"
#include "features/common/FeatureSharedUtils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Converts occurrences into sorted LSP location objects.
 * @param[in] occurrences Raw occurrences collected across documents.
 * @return Stable sorted list of LSP locations.
 */
static std::vector<lsp::Location>
ConvertAndSortOccurrences(const std::vector<analysis::OccurrenceLocation>& occurrences)
{
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
                  return common::CompareByPosition(a.range, b.range);
              });

    return results;
}

/**
 * @brief Get references.
 * @param[in] request Contextual request bundle containing source text and parameters.
 * @return Found item if present; std::nullopt otherwise.
 */
std::optional<ReferencesResult> GetReferences(const ReferencesRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    auto resolveReq = analysis::MakeTargetRequest(
        request.uri, request.sourceCode, analysis::TargetPosition{request.position.line, request.position.character},
        request.symbolTable, request.scopeIndex);
    resolveReq.logger = request.logger;
    resolveReq.useCallBindings = true;
    resolveReq.callBindingRevision = request.callBindingRevision;
    const auto target = analysis::ResolveTargetSymbol(resolveReq);

    if (!target.has_value())
    {
        return std::nullopt;
    }

    auto occReq = analysis::MakeOccurrencesRequest(*target, request.uri, request.sourceCode, request.symbolTable,
                                                   request.scopeIndex);
    occReq.includeDeclaration = request.includeDeclaration;
    occReq.logger = request.logger;
    if (request.occurrenceModule)
    {
        occReq.occurrenceModule.emplace(*request.occurrenceModule);
    }
    const auto occurrences = analysis::CollectOccurrences(occReq);

    if (occurrences.empty())
    {
        return std::nullopt;
    }

    return ConvertAndSortOccurrences(occurrences);
}
} // namespace angel_lsp::features
