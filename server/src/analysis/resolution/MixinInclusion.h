#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "i18n/i18n.h"

#include <ankerl/unordered_dense.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
using MixinInclusionMap =
    ankerl::unordered_dense::map<std::string, std::vector<std::string>, TransparentStringHash, std::equal_to<>>;

/**
 * @brief Context bundling global symbol table and mixin inclusion mapping.
 */
struct MixinClassContext
{
    const SymbolTable& symbolTable;
    const MixinInclusionMap& mixinMap;
};

/**
 * @brief Precomputes mapping from included mixin names to including classes and their derived classes.
 * @param[in] symbolTable Global symbol table.
 * @return Map of mixin name to all compatible host and derived class names.
 */
[[nodiscard]] MixinInclusionMap BuildMixinInclusionMap(const SymbolTable& symbolTable);

/**
 * @brief Collects all classes compatible with member symbol access across mixins and hierarchies.
 * @param[in] symGroup Symbol group sharing declaration range.
 * @param[in] sym Primary symbol.
 * @param[in] targetAccess Access modifier of the member.
 * @param[in] ctx Context bundling mixin map and symbol table.
 * @return Set of compatible class names.
 */
[[nodiscard]] ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>
CollectCompatibleClasses(const std::vector<Symbol>& symGroup, const Symbol& sym, AccessModifier targetAccess,
                         const MixinClassContext& ctx);

/**
 * @brief Collects all mixin names included by a class directly or through inheritance.
 * @param[in] classSym Class symbol to inspect.
 * @param[in] table Global symbol table.
 * @return Vector of unique mixin names.
 */
[[nodiscard]] std::vector<std::string> CollectClassIncludedMixins(const Symbol& classSym, const SymbolTable& table);

/**
 * @brief Locates a mixin symbol definition in the symbol table.
 * @param[in] mixinName Qualified or bare mixin name.
 * @param[in] symbolTable Global symbol table.
 * @return Optional matching mixin Symbol.
 */
[[nodiscard]] std::optional<Symbol> FindMixinSymbol(std::string_view mixinName, const SymbolTable& symbolTable);

/**
 * @brief Finds the declaration range of an included mixin type within a host class scope.
 * @param[in] root Root scope of the document.
 * @param[in] name Mixin type name to search.
 * @param[in] firstLine Starting line of the host class.
 * @param[in] lastLine Ending line of the host class.
 * @return Source range of the type reference if found.
 */
[[nodiscard]] std::optional<SourceRange> FindTypeReferenceRange(const Scope& root, std::string_view name,
                                                                uint32_t firstLine, uint32_t lastLine);

/**
 * @brief Formats localized title for viewing a mixin expansion.
 * @param[in] mixinName Name of the mixin.
 * @param[in] i18n Optional pointer to i18n translation context.
 * @return Formatted command title string.
 */
[[nodiscard]] std::string FormatViewMixinExpansionTitle(std::string_view mixinName, const i18n::I18n* i18n);

/**
 * @brief Formats localized title for jumping to physical mixin source.
 * @param[in] fileUri URI or path to the physical mixin source.
 * @param[in] i18n Optional pointer to i18n translation context.
 * @return Formatted command title string.
 */
[[nodiscard]] std::string FormatJumpToPhysicalSourceTitle(std::string_view fileUri, const i18n::I18n* i18n);
} // namespace angel_lsp::analysis
