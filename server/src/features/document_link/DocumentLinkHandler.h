#pragma once

#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"

#include <lsp/types.h>

#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::features
{
    /**
     * @brief Input for both `textDocument/documentLink` and the unresolved-include check.
     */
    struct DocumentLinkRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        const std::vector<std::string> &searchDirectories;
        const angel_lsp::i18n::I18n *i18n = nullptr;
    };

    using DocumentLinkResult = std::vector<lsp::DocumentLink>;

    /**
     * @brief Turns every resolvable `#include` directive into a clickable link.
     *
     * Ranges cover just the path inside the quotes or angle brackets, in byte columns like every
     * other handler; the server converts them to the client's encoding on the way out.
     *
     * @param request Document and include search paths.
     * @return One link per resolvable directive, or std::nullopt when the document has none.
     */
    std::optional<DocumentLinkResult> GetDocumentLinks(const DocumentLinkRequest &request);

    /**
     * @brief Reports `#include` directives that resolve to no file on disk.
     *
     * A mistyped or stale include is invisible until something downstream fails to find a symbol,
     * and it silently drops the file out of its module (see WorkspaceIncludeGraph), so it is worth
     * saying plainly where the problem is.
     *
     * @param request Document and include search paths.
     * @return One warning per unresolvable directive.
     */
    std::vector<analysis::Diagnostic> GetUnresolvedIncludeDiagnostics(const DocumentLinkRequest &request);
}
