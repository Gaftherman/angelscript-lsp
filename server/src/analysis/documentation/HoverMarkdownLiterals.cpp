#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/resolution/HoverResolution.h"
#include "i18n/i18n.h"
#include "utils/Utils.h"

#include <filesystem>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::analysis
{
std::string FormatIncludeDirectiveHover(std::string_view rawPath, const HoverDocumentSources& sources,
                                        const angel_lsp::i18n::I18n* i18n)
{
    std::string markdown = "```angelscript\n#include \"" + std::string(rawPath) + "\"\n```";

    if (sources.resolveInclude)
    {
        const std::string resolved = sources.resolveInclude(rawPath);
        if (resolved.empty())
        {
            markdown += "\n\n" + angel_lsp::i18n::FormatMessage(
                                     i18n, "hover-include-unresolved",
                                     "Does not resolve to a file. Checked this file's own directory, then each "
                                     "`angelscript.searchDirectories` entry in order.");
        }
        else
        {
            markdown += "\n\n" + resolved;
        }
    }
    return markdown;
}

std::string FormatPrimitiveTypeHover(std::string_view nodeText, const angel_lsp::i18n::I18n* i18n)
{
    std::string md = "```angelscript\n(primitive type) " + std::string(nodeText) + "\n```";
    std::string doc = FormatPrimitiveDoc(nodeText, i18n);
    if (!doc.empty())
    {
        md += "\n\n" + doc;
    }
    return md;
}

std::string FormatWildcardTypeHover(const angel_lsp::i18n::I18n* i18n)
{
    std::string desc = angel_lsp::i18n::FormatMessage(
        i18n, "hover-wildcard-type-desc",
        "The variable type `?` represents any type. It is used in parameter declarations "
        "to accept arguments of any type by reference (`?& in`, `?& out`, `?& inout`).");
    return "```angelscript\n(variable type) ?\n```\n\n" + desc;
}

std::string FormatThisHover(std::string_view className, std::string_view nodeText)
{
    return fmt::format("```angelscript\n(self) {} {}\n```", className, nodeText);
}

std::string FormatStringLiteralHover(std::string_view txt, std::string_view unquoted, bool showLength,
                                     const angel_lsp::i18n::I18n* i18n)
{
    std::string md = "```angelscript\n" + std::string(txt) + "\n```";
    if (showLength)
    {
        md += "\n\n" + angel_lsp::i18n::FormatMessage(i18n, "hover-string-length", "- **Length**: {} characters",
                                                      unquoted.size());
    }
    return md;
}

std::string ResolveStringLiteralPath(std::string_view unquoted, std::string_view uri,
                                     const HoverDocumentSources& sources, const config::ServerConfig* config)
{
    std::string unquotedStr(unquoted);
    if (sources.resolveInclude)
    {
        std::string resolved = sources.resolveInclude(unquotedStr);
        if (!resolved.empty())
        {
            return resolved;
        }
    }

    std::error_code ec;
    std::filesystem::path docPath(utils::UriToPath(std::string(uri)));
    std::filesystem::path candidate = docPath.parent_path() / unquotedStr;
    if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec))
    {
        return candidate.lexically_normal().string();
    }

    if (config)
    {
        for (const auto& searchDir : config->features.assetSearchPaths)
        {
            std::filesystem::path assetPath = std::filesystem::path(searchDir) / unquotedStr;
            if (std::filesystem::exists(assetPath, ec) && !std::filesystem::is_directory(assetPath, ec))
            {
                return assetPath.lexically_normal().string();
            }
        }
    }
    return "";
}

bool IsPathLikeString(std::string_view str)
{
    if (str.find('/') != std::string_view::npos || str.find('\\') != std::string_view::npos)
    {
        return true;
    }
    auto dot = str.rfind('.');
    if (dot != std::string_view::npos && dot + 1 < str.size())
    {
        std::string_view ext = str.substr(dot + 1);
        if (ext.size() >= 2 && ext.size() <= 4)
        {
            return true;
        }
    }
    return false;
}

std::string FormatStringPathHover(std::string_view path, bool showLength, const HoverDocLookupContext& docCtx)
{
    if (!docCtx.sources || !IsPathLikeString(path))
    {
        return "";
    }
    const std::string resolved = ResolveStringLiteralPath(path, docCtx.documentUri, *docCtx.sources, docCtx.config);
    if (resolved.empty())
    {
        return "";
    }
    return FormatResolvedAssetMarkdown(path, resolved, showLength, docCtx.i18n);
}

std::string FormatConcatenatedPathHover(IndexedByteRange plusRange, const ExpressionTypeContext& exprCtx,
                                        bool showLength, const HoverDocLookupContext& docCtx)
{
    const std::optional<std::string> evalStr =
        EvaluateConstantStringExpr(plusRange.startByte, plusRange.endByte, exprCtx);
    return evalStr ? FormatStringPathHover(*evalStr, showLength, docCtx) : std::string{};
}
} // namespace angel_lsp::analysis
