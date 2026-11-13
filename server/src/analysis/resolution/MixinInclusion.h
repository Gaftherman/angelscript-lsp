#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "i18n/i18n.h"

#include <ankerl/unordered_dense.h>
#include <optional>
#include <span>
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
CollectCompatibleClasses(std::span<const Symbol> symGroup, const Symbol& sym, AccessModifier targetAccess,
                         const MixinClassContext& ctx);

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

} // namespace angel_lsp::analysis
