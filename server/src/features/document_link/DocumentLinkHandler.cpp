#include "features/document_link/DocumentLinkHandler.h"

#include "analysis/diagnostics/DiagnosticCodes.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"

namespace angel_lsp::features
{
/**
 * @brief Byte span of the path inside an `#include` directive, excluding its delimiters.
 * @param[in] line Source text line containing the include directive.
 * @param[in] isAngled True if delimiters are <>, false if "".
 * @return Start and end byte columns, or std::nullopt when no delimited path is present.
 */
static std::optional<std::pair<uint32_t, uint32_t>> FindPathSpan(std::string_view line, bool isAngled)
{
    const size_t hash = line.find('#');
    if (hash == std::string_view::npos)
    {
        return std::nullopt;
    }

    const size_t incPos = line.find("include", hash + 1);
    if (incPos == std::string_view::npos)
    {
        return std::nullopt;
    }

    const char open = isAngled ? '<' : '"';
    const char close = isAngled ? '>' : '"';

    const size_t openPos = line.find(open, incPos + 7);
    if (openPos == std::string_view::npos)
    {
        return std::nullopt;
    }

    const size_t closePos = line.find(close, openPos + 1);
    if (closePos == std::string_view::npos)
    {
        return std::nullopt;
    }

    return std::make_pair(static_cast<uint32_t>(openPos + 1), static_cast<uint32_t>(closePos));
}

/**
 * @brief Filesystem form of the LSP document URI being inspected, or empty for a non-file URI.
 * @param[in] uriStr Source URI string view.
 * @return Normalized file system path decoded from the protocol URI, or empty string.
 */
static lsp::String DocumentPath(std::string_view uriStr)
{
    const lsp::Uri uri = lsp::Uri::parse(uriStr);
    if (!uri.isValid() || !uri.isFileUri())
    {
        return "";
    }

    return utils::IncludeResolver::NormalizePath(uri.fsPath());
}

/**
 * @brief Builds an include resolution request bundle for a directive path.
 * @param[in] request Parent document link request.
 * @param[in] currentPath Normalized file system path of the host document.
 * @param[in] includePath Target include path from directive.
 * @return Assembled IncludeResolveRequest.
 */
static utils::IncludeResolveRequest MakeResolveRequest(const DocumentLinkRequest& request, std::string_view currentPath,
                                                       std::string_view includePath) noexcept
{
    return utils::IncludeResolveRequest{
        .includePath = includePath,
        .currentFilePath = currentPath,
        .searchDirectories = request.includeScope.searchDirectories,
        .allowedRoots = request.includeScope.allowedRoots,
        .implicitExtension = request.includeScope.implicitExtension,
    };
}

/**
 * @brief Turns every resolvable `#include` directive into a clickable link.
 * @param[in] request Document link request bundle containing URI, source text, and search paths.
 * @return One link per resolvable directive, or std::nullopt when the document has none.
 */
std::optional<DocumentLinkResult> GetDocumentLinks(const DocumentLinkRequest& request)
{
    const auto currentPath = DocumentPath(request.uri);
    DocumentLinkResult links;

    if (request.nodeIndex)
    {
        const auto directives = request.nodeIndex->IncludeDirectives();
        links.reserve(directives.size());
        for (const auto& directive : directives)
        {
            const auto target =
                utils::IncludeResolver::ResolveIncludePath(MakeResolveRequest(request, currentPath, directive.rawPath));
            if (target.empty())
            {
                continue;
            }
            links.push_back(lsp::DocumentLink{
                .range =
                    {
                        .start = {.line = directive.line, .character = directive.spanStart},
                        .end = {.line = directive.line, .character = directive.spanEnd},
                    },
                .target = lsp::Uri::fileUriFromPath(target),
            });
        }
        return links.empty() ? std::nullopt : std::optional<DocumentLinkResult>{std::move(links)};
    }

    const auto directives = utils::IncludeResolver::ExtractIncludes(request.sourceCode);
    if (directives.empty())
    {
        return std::nullopt;
    }

    links.reserve(directives.size());

    for (const auto& directive : directives)
    {
        const auto target =
            utils::IncludeResolver::ResolveIncludePath(MakeResolveRequest(request, currentPath, directive.rawPath));
        if (target.empty())
        {
            continue;
        }

        const std::string_view line = utils::GetLine(request.sourceCode, static_cast<uint32_t>(directive.line));
        const auto span = FindPathSpan(line, directive.isAngled);
        if (!span.has_value())
        {
            continue;
        }

        links.push_back(lsp::DocumentLink{
            .range =
                {
                    .start = {.line = static_cast<uint32_t>(directive.line), .character = span->first},
                    .end = {.line = static_cast<uint32_t>(directive.line), .character = span->second},
                },
            .target = lsp::Uri::fileUriFromPath(target),
        });
    }

    if (links.empty())
    {
        return std::nullopt;
    }

    return links;
}

/**
 * @brief Builds the localized "include not found" warning for one directive.
 * @param[in] request Document link request providing the URI and localization provider.
 * @param[in] range Span of the include path inside the directive.
 * @param[in] rawPath Include path exactly as written.
 * @return Warning diagnostic owned by the caller.
 */
static analysis::Diagnostic MakeIncludeNotFoundDiagnostic(const DocumentLinkRequest& request,
                                                          const analysis::DiagnosticRange& range,
                                                          std::string_view rawPath)
{
    analysis::Diagnostic diagnostic{
        .range = range,
        .severity = analysis::DiagnosticSeverity::Warning,
        .message = i18n::FormatMessage(request.i18n, diagnostics::codes::IncludeNotFound,
                                       "Included file '{}' was not found.", rawPath),
    };
    diagnostic.code = diagnostics::codes::IncludeNotFound;
    diagnostic.fileUri = request.uri;
    return diagnostic;
}

/**
 * @brief Collects unresolved include diagnostics by parsing raw source text fallback.
 * @param[in] request Document link request bundle.
 * @param[in] currentPath Canonical file path of active document.
 * @param[in,out] out Diagnostics destination vector.
 */
static void CollectFallbackUnresolvedDiagnostics(const DocumentLinkRequest& request, std::string_view currentPath,
                                                 std::vector<analysis::Diagnostic>& out)
{
    const auto directives = utils::IncludeResolver::ExtractIncludes(request.sourceCode, request.excludedLineRanges);
    for (const auto& directive : directives)
    {
        if (!utils::IncludeResolver::ResolveIncludePath(MakeResolveRequest(request, currentPath, directive.rawPath))
                 .empty())
        {
            continue;
        }

        const std::string_view line = utils::GetLine(request.sourceCode, static_cast<uint32_t>(directive.line));
        const auto span = FindPathSpan(line, directive.isAngled);
        if (!span.has_value())
        {
            continue;
        }

        out.push_back(MakeIncludeNotFoundDiagnostic(
            request,
            {
                .start = {.line = static_cast<uint32_t>(directive.line), .character = span->first},
                .end = {.line = static_cast<uint32_t>(directive.line), .character = span->second},
            },
            directive.rawPath));
    }
}

/**
 * @brief Reports `#include` directives that resolve to no file on disk.
 * @param[in] request Document link request bundle containing URI, source text, and search paths.
 * @return Vector of diagnostics for include directives that could not be resolved to files.
 */
std::vector<analysis::Diagnostic> GetUnresolvedIncludeDiagnostics(const DocumentLinkRequest& request)
{
    std::vector<analysis::Diagnostic> diagnostics;
    const auto currentPath = DocumentPath(request.uri);

    if (request.nodeIndex)
    {
        for (const auto& directive : request.nodeIndex->IncludeDirectives())
        {
            if (utils::IsLineExcluded(request.excludedLineRanges, directive.line))
            {
                continue;
            }
            if (!utils::IncludeResolver::ResolveIncludePath(MakeResolveRequest(request, currentPath, directive.rawPath))
                     .empty())
            {
                continue;
            }

            diagnostics.push_back(
                MakeIncludeNotFoundDiagnostic(request,
                                              {
                                                  .start = {.line = directive.line, .character = directive.spanStart},
                                                  .end = {.line = directive.line, .character = directive.spanEnd},
                                              },
                                              directive.rawPath));
        }
        return diagnostics;
    }

    CollectFallbackUnresolvedDiagnostics(request, currentPath, diagnostics);
    return diagnostics;
}
} // namespace angel_lsp::features
