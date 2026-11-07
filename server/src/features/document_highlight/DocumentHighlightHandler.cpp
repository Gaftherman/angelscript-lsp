#include "features/document_highlight/DocumentHighlightHandler.h"

#include "analysis/resolution/OccurrenceAccess.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TargetResolution.h"
#include "features/common/FeatureSharedUtils.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Maps a Layer 2 access classification onto the LSP highlight kind shown by the editor.
 * @param[in] access Classification of one occurrence.
 * @return `Write` and `Text` map one-to-one; every other value is presented as `Read`.
 */
static lsp::DocumentHighlightKind ToHighlightKind(analysis::OccurrenceAccess access) noexcept
{
    switch (access)
    {
    case analysis::OccurrenceAccess::Write:
        return lsp::DocumentHighlightKind::Write;
    case analysis::OccurrenceAccess::Text:
        return lsp::DocumentHighlightKind::Text;
    case analysis::OccurrenceAccess::Read:
    default:
        return lsp::DocumentHighlightKind::Read;
    }
}

std::optional<DocumentHighlightResult> GetDocumentHighlights(const DocumentHighlightRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    auto resolveReq = analysis::MakeTargetRequest(
        request.uri, request.sourceCode, analysis::TargetPosition{request.position.line, request.position.character},
        request.symbolTable, request.scopeIndex);
    auto targetOpt = analysis::ResolveTargetSymbol(resolveReq);
    if (!targetOpt.has_value())
    {
        return std::nullopt;
    }

    auto occReq = analysis::MakeOccurrencesRequest(*targetOpt, request.uri, request.sourceCode, request.symbolTable,
                                                   request.scopeIndex);
    occReq.includeDeclaration = true;
    const auto occurrences = analysis::CollectOccurrences(occReq);

    std::vector<analysis::SourceRange> ranges;
    for (const auto& occ : occurrences)
    {
        if (occ.fileUri == request.uri)
        {
            ranges.push_back(occ.range);
        }
    }
    if (ranges.empty())
    {
        return std::nullopt;
    }

    std::ranges::sort(ranges, {},
                      [](const analysis::SourceRange& r) { return std::pair{r.startLine, r.startCharacter}; });
    ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());

    const analysis::OccurrenceAccessClassifier classifier(request.uri, request.sourceCode, request.symbolTable,
                                                          request.scopeIndex);

    DocumentHighlightResult results;
    results.reserve(ranges.size());
    for (const auto& r : ranges)
    {
        results.push_back(lsp::DocumentHighlight{
            .range = common::ToLspRange(r),
            .kind = ToHighlightKind(classifier.Classify(*targetOpt, r)),
        });
    }

    return results;
}

} // namespace angel_lsp::features
