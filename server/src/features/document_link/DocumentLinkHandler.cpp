#include "features/document_link/DocumentLinkHandler.h"

#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"

#include <spdlog/fmt/fmt.h>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Byte span of the path inside an `#include` directive, excluding its delimiters.
         *
         * IncludeDirective records which line a directive is on but not where on it, and the
         * directive may be indented or spaced any way at all, so the delimiters are located by
         * scanning the line rather than assuming a column.
         *
         * @return Start and end byte columns, or std::nullopt when the line holds no delimited path.
         */
        std::optional<std::pair<uint32_t, uint32_t>> FindPathSpan(std::string_view line, bool isAngled)
        {
            const char open = isAngled ? '<' : '"';
            const char close = isAngled ? '>' : '"';

            const size_t openPos = line.find(open);
            if (openPos == std::string_view::npos)
                return std::nullopt;

            const size_t closePos = line.find(close, openPos + 1);
            if (closePos == std::string_view::npos)
                return std::nullopt;

            return std::make_pair(static_cast<uint32_t>(openPos + 1), static_cast<uint32_t>(closePos));
        }

        /**
         * @brief Filesystem path of the document being inspected, or empty for a non-file URI.
         */
        std::string DocumentPath(const std::string &uriStr)
        {
            const lsp::Uri uri = lsp::Uri::parse(uriStr);
            if (!uri.isValid() || !uri.isFileUri())
                return "";

            return utils::IncludeResolver::NormalizePath(uri.fsPath());
        }
    }

    std::optional<DocumentLinkResult> GetDocumentLinks(const DocumentLinkRequest &request)
    {
        const auto directives = utils::IncludeResolver::ExtractIncludes(request.sourceCode);
        if (directives.empty())
            return std::nullopt;

        const std::string currentPath = DocumentPath(request.uri);

        DocumentLinkResult links;
        links.reserve(directives.size());

        for (const auto &directive : directives)
        {
            const std::string target = utils::IncludeResolver::ResolveIncludePath(
                directive.rawPath, currentPath, request.searchDirectories);
            if (target.empty())
                continue;

            const std::string_view line = utils::GetLine(request.sourceCode, static_cast<uint32_t>(directive.line));
            const auto span = FindPathSpan(line, directive.isAngled);
            if (!span.has_value())
                continue;

            lsp::DocumentLink link;
            link.range.start.line = static_cast<uint32_t>(directive.line);
            link.range.start.character = span->first;
            link.range.end.line = static_cast<uint32_t>(directive.line);
            link.range.end.character = span->second;
            link.target = lsp::Uri::fileUriFromPath(target);
            link.tooltip = target;

            links.push_back(std::move(link));
        }

        if (links.empty())
            return std::nullopt;

        return links;
    }

    std::vector<analysis::Diagnostic> GetUnresolvedIncludeDiagnostics(const DocumentLinkRequest &request)
    {
        std::vector<analysis::Diagnostic> diagnostics;

        const auto directives = utils::IncludeResolver::ExtractIncludes(request.sourceCode);
        if (directives.empty())
            return diagnostics;

        const std::string currentPath = DocumentPath(request.uri);

        for (const auto &directive : directives)
        {
            if (!utils::IncludeResolver::ResolveIncludePath(directive.rawPath, currentPath, request.searchDirectories).empty())
                continue;

            const std::string_view line = utils::GetLine(request.sourceCode, static_cast<uint32_t>(directive.line));
            const auto span = FindPathSpan(line, directive.isAngled);
            if (!span.has_value())
                continue;

            analysis::Diagnostic diagnostic;
            diagnostic.range.start.line = static_cast<uint32_t>(directive.line);
            diagnostic.range.start.character = span->first;
            diagnostic.range.end.line = static_cast<uint32_t>(directive.line);
            diagnostic.range.end.character = span->second;
            diagnostic.severity = analysis::DiagnosticSeverity::Warning;
            diagnostic.code = "as-warn-include-not-found";
            diagnostic.fileUri = request.uri;

            const std::string format = request.i18n
                                           ? request.i18n->GetMessage(diagnostic.code)
                                           : std::string("Included file '{}' was not found.");
            diagnostic.message = fmt::format(fmt::runtime(format), directive.rawPath);

            diagnostics.push_back(std::move(diagnostic));
        }

        return diagnostics;
    }
}
