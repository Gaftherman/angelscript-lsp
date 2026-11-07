#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "i18n/i18n.h"

#include <ankerl/unordered_dense.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Line span metadata for a class or interface definition within a document.
 */
struct EnclosingClassSpan
{
    uint32_t startLine = 0;
    uint32_t endLine = 0;
    std::string name;
};

using EnclosingClassCache =
    ankerl::unordered_dense::map<std::string, std::vector<EnclosingClassSpan>, TransparentStringHash, std::equal_to<>>;

using DeclRangeSet = ankerl::unordered_dense::set<std::tuple<std::string, uint32_t, uint32_t>>;

using TargetIndexMap =
    ankerl::unordered_dense::map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>>;

using ModuleFilter = std::optional<std::string>;

/**
 * @brief Target symbol and range for computing cross-document reference counts.
 */
struct ReferenceCountTarget
{
    SourceRange key;
    std::string targetName;
    const std::vector<Symbol>& group;
    ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>> compatibleClasses;
    DeclRangeSet allDeclRanges;
    std::string targetNamespace;
    std::string targetEnumContainer;
    bool isEnumConstant = false;
    AccessModifier targetAccess = AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenRefs;
};

/**
 * @brief Context and input parameters for computing reference counts across a workspace.
 */
struct ReferenceCountRequest
{
    std::string_view uri;
    std::string_view sourceCode;
    const SymbolTable& symbolTable;
    const ScopeIndex& scopeIndex;
    std::string_view predefinedExtension = ".as.predefined";
    const ModuleFilter* occurrenceModule = nullptr;
};

/**
 * @brief Filter criteria for finding matching references in scope trees.
 */
struct ReferenceCollectionCriteria
{
    std::string_view targetName;
    const std::vector<Symbol>& group;
    const ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>& compatibleClasses;
    const DeclRangeSet& allDeclRanges;
    std::string_view targetNamespace;
    std::string_view targetEnumContainer;
    bool isEnumConstant = false;
    AccessModifier targetAccess = AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    const SymbolTable& symbolTable;
    const ReferenceCountRequest& request;
    EnclosingClassCache& classCache;
};

/**
 * @brief Parameters for batched cross-document reference collection across scopes.
 */
struct BatchReferenceContext
{
    BatchReferenceContext(std::span<ReferenceCountTarget> t, const TargetIndexMap& map,
                          const ReferenceCountRequest& req)
        : targets(t), targetsByName(map), request(req)
    {
    }

    std::span<ReferenceCountTarget> targets;
    const TargetIndexMap& targetsByName;
    const ReferenceCountRequest& request;
    mutable EnclosingClassCache classCache;
};

/**
 * @brief Counts references across all documents in a single pass over the scope index.
 * @param[in,out] targets Vector of reference targets to update.
 * @param[in] targetsByName Mapping from symbol name to target indices.
 * @param[in] request Active reference count request.
 */
void CountReferences(std::span<ReferenceCountTarget> targets, const TargetIndexMap& targetsByName,
                     const ReferenceCountRequest& request);

/**
 * @brief Formats a localized reference count string for CodeLens.
 * @param[in] refCount Number of references.
 * @param[in] i18n Optional pointer to I18n translation context.
 * @return Formatted reference count message.
 */
[[nodiscard]] std::string FormatReferenceCount(size_t refCount, const i18n::I18n* i18n);

/**
 * @brief Adds declaration line and character coordinates of symbols to a range set.
 * @param[in] syms List of symbols.
 * @param[in,out] declRanges Output set of declaration coordinates.
 */
void AddSymbolDeclRanges(const std::vector<Symbol>& syms, DeclRangeSet& declRanges);

/**
 * @brief Collects all declaration coordinates for a symbol and its compatible class variants.
 * @param[in] symName Member symbol name.
 * @param[in] symGroup Symbol group sharing declaration range.
 * @param[in] compatibleClasses Set of compatible classes.
 * @param[in] symbolTable Global symbol table.
 * @return Populated set of declaration coordinate tuples.
 */
[[nodiscard]] DeclRangeSet CollectAllDeclRanges(
    std::string_view symName, const std::vector<Symbol>& symGroup,
    const ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>& compatibleClasses,
    const SymbolTable& symbolTable);

/**
 * @brief Computes the target namespace for a symbol target.
 * @param[in] isEnumConstant Whether the target is an enum constant.
 * @param[in] targetEnumContainer Enum container name if target is an enum constant.
 * @param[in] sym Primary symbol.
 * @param[in] table Global symbol table.
 * @return Computed namespace name.
 */
[[nodiscard]] std::string ComputeTargetNamespace(bool isEnumConstant, std::string_view targetEnumContainer,
                                                 const Symbol& sym, const SymbolTable& table);

/**
 * @brief Computes the enum container type name for a variable symbol if applicable.
 * @param[in] sym Primary symbol.
 * @return Container name or empty string.
 */
[[nodiscard]] std::string ComputeEnumContainer(const Symbol& sym);
} // namespace angel_lsp::analysis
