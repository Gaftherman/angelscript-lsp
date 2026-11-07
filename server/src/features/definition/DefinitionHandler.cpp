#include "features/definition/DefinitionHandler.h"

#include "analysis/index/Symbol.h"
#include "analysis/resolution/DefinitionResolution.h"
#include "analysis/resolution/TargetResolution.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Formats an LSP Location from a symbol definition.
 * @param[in] sym Target symbol.
 * @param[in] query Definition query context.
 * @return Formatted LSP Location with appropriate URI and mapped line range.
 */
static lsp::Location MakeLocation(const analysis::Symbol& sym, const analysis::DefinitionQuery& query)
{
    const auto mapped = analysis::MapVirtualMixinLocation(sym, query.uri, query.symbolTable);
    return lsp::Location{common::ParseOrConvertUri(mapped.targetUri), common::ToLspRange(mapped.range)};
}

/**
 * @brief Converts symbol candidates to deduplicated LSP locations.
 * @param[in] symbols Candidate symbols span.
 * @param[in] query Definition query context.
 * @return Formatted and deduplicated LSP Location vector.
 */
static std::vector<lsp::Location> ConvertSymbolsToLocations(std::span<const analysis::Symbol> symbols,
                                                            const analysis::DefinitionQuery& query)
{
    std::vector<lsp::Location> locations;
    bool hasWorkspaceSymbol = false;
    for (const auto& sym : symbols)
    {
        if (sym.type != analysis::SymbolType::CallReference &&
            !utils::IsPredefinedFile(sym.fileUri, query.predefinedExtension))
        {
            hasWorkspaceSymbol = true;
            break;
        }
    }

    for (const auto& sym : symbols)
    {
        if (sym.type != analysis::SymbolType::CallReference)
        {
            if (hasWorkspaceSymbol && utils::IsPredefinedFile(sym.fileUri, query.predefinedExtension))
            {
                continue;
            }
            auto loc = MakeLocation(sym, query);
            bool duplicate = false;
            for (const auto& existing : locations)
            {
                if (existing.uri.toString() == loc.uri.toString() && common::SameRange(existing.range, loc.range))
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                locations.push_back(std::move(loc));
            }
        }
    }
    return locations;
}

/**
 * @brief Attempts to resolve definition when cursor is within an `#include` directive.
 * @param[in] request Definition request context.
 * @return Location vector pointing to the resolved include file, or nullopt.
 */
static std::optional<std::vector<lsp::Location>> TryResolveIncludeDirective(const DefinitionRequest& request)
{
    if (!request.resolveInclude)
    {
        return std::nullopt;
    }

    const std::string_view line = common::ExtractLineAt(request.query.sourceCode, request.query.line);
    if (line.empty())
    {
        return std::nullopt;
    }

    const auto info =
        angel_lsp::utils::IncludeResolver::ParseSingleLineInclude(line, static_cast<size_t>(request.query.character));
    if (!info || info->rawPath.empty())
    {
        return std::nullopt;
    }

    auto targetUri = request.resolveInclude(info->rawPath);
    if (!targetUri.has_value())
    {
        return std::nullopt;
    }

    return std::vector<lsp::Location>{
        lsp::Location{std::move(*targetUri), lsp::Range{lsp::Position{0, 0}, lsp::Position{0, 0}}}};
}

std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest& request)
{
    if (request.query.sourceCode.empty())
    {
        return std::nullopt;
    }

    if (auto includeLoc = TryResolveIncludeDirective(request))
    {
        return includeLoc;
    }

    auto query = request.query;
    if (query.identifier.empty())
    {
        query.identifier =
            common::FindIdentifierAtPosition(query.sourceCode, lsp::Position{query.line, query.character});
    }
    if (query.identifier.empty() || !analysis::IsValidIdentifier(query.identifier))
    {
        return std::nullopt;
    }
    if (query.scopedIdentifier.empty())
    {
        query.scopedIdentifier =
            common::FindScopedIdentifierAtPosition(query.sourceCode, lsp::Position{query.line, query.character});
    }

    auto target = analysis::ResolveDefinitionTarget(query);
    if (target.localRange.has_value())
    {
        return std::vector<lsp::Location>{
            lsp::Location{common::ParseOrConvertUri(target.localUri), common::ToLspRange(*target.localRange)}};
    }
    if (target.callDeclaration.has_value())
    {
        return std::vector<lsp::Location>{lsp::Location{common::ParseOrConvertUri(target.callDeclaration->uri),
                                                        common::ToLspRange(target.callDeclaration->range)}};
    }
    if (!target.symbols.empty())
    {
        auto locs = ConvertSymbolsToLocations(target.symbols, query);
        if (!locs.empty())
        {
            return locs;
        }
    }

    return std::nullopt;
}

std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest& request)
{
    if (request.query.sourceCode.empty())
    {
        return std::nullopt;
    }

    auto query = request.query;
    if (query.identifier.empty())
    {
        query.identifier =
            common::FindIdentifierAtPosition(query.sourceCode, lsp::Position{query.line, query.character});
    }
    if (query.identifier.empty() || !analysis::IsValidIdentifier(query.identifier))
    {
        return std::nullopt;
    }
    if (query.scopedIdentifier.empty())
    {
        query.scopedIdentifier =
            common::FindScopedIdentifierAtPosition(query.sourceCode, lsp::Position{query.line, query.character});
    }

    auto symbols = analysis::ResolveTypeDefinitionTarget(query);
    if (symbols.empty())
    {
        return std::nullopt;
    }

    auto locs = ConvertSymbolsToLocations(symbols, query);
    return locs.empty() ? std::nullopt : std::make_optional(std::move(locs));
}

} // namespace angel_lsp::features
