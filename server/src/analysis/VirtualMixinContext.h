#pragma once

#include "analysis/SymbolTable.h"
#include <optional>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Context extracted from virtual mixin document URIs.
 */
struct VirtualMixinContext
{
    bool isVirtual = false;
    std::string hostClass;
    std::string mixinName;
    std::optional<Symbol> mixinSymbol;
};

/**
 * @brief Resolves virtual mixin context metadata from a document URI and symbol table.
 * @param[in] uri Document URI string.
 * @param[in] symbolTable Global symbol table for mixin symbol lookup.
 * @return Populated VirtualMixinContext.
 */
[[nodiscard]] inline VirtualMixinContext ResolveVirtualMixinContext(std::string_view uri,
                                                                    const SymbolTable& symbolTable)
{
    VirtualMixinContext ctx;
    ctx.isVirtual = uri.starts_with("angelscript-virtual:") || uri.starts_with("angelscript-virtual://");
    if (!ctx.isVirtual)
    {
        return ctx;
    }

    ctx.hostClass = SymbolTable::ExtractVirtualHostClass(uri);
    ctx.mixinName = SymbolTable::ExtractVirtualMixinName(uri);

    auto candidates = symbolTable.FindSymbolsPtr(ctx.mixinName);
    if (candidates)
    {
        for (const auto& cand : *candidates)
        {
            if (cand.type == SymbolType::Class)
            {
                ctx.mixinSymbol = cand;
                break;
            }
        }
    }
    if (!ctx.mixinSymbol.has_value())
    {
        std::string shortName = std::string(LastScopeSegment(ctx.mixinName));
        auto shortCandidates = symbolTable.FindTypeSymbolsByShortName(shortName);
        for (const auto& cand : shortCandidates)
        {
            if (cand.type == SymbolType::Class)
            {
                ctx.mixinSymbol = cand;
                break;
            }
        }
    }
    return ctx;
}

/**
 * @brief Resolves the declaring document URI for mixin scopes, falling back to document URI.
 * @param[in] ctx Virtual mixin context.
 * @param[in] fallbackUri Original document URI.
 * @return Declaring file URI or fallbackUri.
 */
[[nodiscard]] inline const std::string& ResolvePhysicalUri(const VirtualMixinContext& ctx,
                                                           const std::string& fallbackUri) noexcept
{
    return (ctx.isVirtual && ctx.mixinSymbol.has_value()) ? ctx.mixinSymbol->fileUri : fallbackUri;
}

/**
 * @brief Maps virtual line number to declaring physical line if in a mixin context.
 * @param[in] ctx Virtual mixin context.
 * @param[in] line Document line number.
 * @return Physical line number in the declaring file.
 */
[[nodiscard]] inline uint32_t ResolvePhysicalLine(const VirtualMixinContext& ctx, uint32_t line) noexcept
{
    return (ctx.isVirtual && ctx.mixinSymbol.has_value())
               ? SymbolTable::VirtualToPhysicalLine(line, ctx.mixinSymbol->startLine)
               : line;
}

} // namespace angel_lsp::analysis
