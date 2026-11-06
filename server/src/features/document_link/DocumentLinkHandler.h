#pragma once

#include "analysis/diagnostics/Diagnostics.h"
#include "analysis/index/NodeIndex.h"
#include "i18n/i18n.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"

#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Input for both `textDocument/documentLink` and the unresolved-include check.
 */
struct DocumentLinkRequest
{
    /**
     * @brief Constructs a DocumentLinkRequest.
     * @param[in] u Source document URI.
     * @param[in] sc Document source code.
     * @param[in] scope Include search directories, confinement roots and implicit extension; borrowed.
     * @param[in] i18nPtr Optional localization provider pointer.
     */
    DocumentLinkRequest(std::string_view u, std::string_view sc, utils::IncludeSearchScope scope,
                        const angel_lsp::i18n::I18n* i18nPtr = nullptr)
        : uri(u), sourceCode(sc), includeScope(scope), i18n(i18nPtr)
    {
    }

    std::string_view uri;
    std::string_view sourceCode;

    /**
     * @brief Where includes are searched and confined; the caller owns the lists it points at.
     */
    utils::IncludeSearchScope includeScope;
    const angel_lsp::i18n::I18n* i18n = nullptr;

    /**
     * @brief Lines the preprocessor removes, so a directive inside one is not a directive.
     */
    std::vector<utils::ExcludedLineRange> excludedLineRanges;

    /**
     * @brief Optional pre-computed syntax node index from pass 1.
     */
    const analysis::NodeIndex* nodeIndex = nullptr;
};

using DocumentLinkResult = std::vector<lsp::DocumentLink>;

/**
 * @brief Turns every resolvable `#include` directive into a clickable link.
 * @param[in] request Document and include search paths.
 * @return One link per resolvable directive, or std::nullopt when the document has none.
 */
std::optional<DocumentLinkResult> GetDocumentLinks(const DocumentLinkRequest& request);

/**
 * @brief Reports `#include` directives that resolve to no file on disk.
 * @param[in] request Document and include search paths.
 * @return One warning per unresolvable directive.
 */
std::vector<analysis::Diagnostic> GetUnresolvedIncludeDiagnostics(const DocumentLinkRequest& request);
} // namespace angel_lsp::features
