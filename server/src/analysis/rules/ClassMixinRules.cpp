#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/MixinSourceIndex.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/types/TypeSanitization.h"
#include "document/Document.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Collects all mixins included via includedMixins or bases.
 * @param[in] sig Class signature providing mixin names and base types.
 * @param[in] table Symbol table used to resolve mixin status.
 * @return Views into sig.includedMixins and bases identifying included mixin types.
 */
static std::vector<std::string_view> CollectIncludedMixins(const ClassSignature& sig, const SymbolTable& table)
{
    std::vector<std::string_view> includedMixins;
    includedMixins.reserve(sig.includedMixins.size() + sig.bases.size());
    for (const auto& mixin : sig.includedMixins)
    {
        includedMixins.push_back(mixin);
    }
    for (const auto& b : sig.bases)
    {
        const std::string_view clean = CleanBaseTypeView(b);
        if (!clean.empty() && IsMixinClass(clean, table))
        {
            if (std::find(includedMixins.begin(), includedMixins.end(), clean) == includedMixins.end())
            {
                includedMixins.push_back(clean);
            }
        }
    }
    return includedMixins;
}

/**
 * @brief Collects member names for a container and its qualified/short variants.
 * @param[in] containerName Identifier of the container class.
 * @param[in] ruleIndex Precalculated index of class and container members.
 * @param[out] outMembers Destination set receiving aggregated member names.
 */
static void CollectContainerMembers(std::string_view containerName, const RuleIndex& ruleIndex,
                                    MemberNameSet& outMembers)
{
    const auto insertMembers = [&](const auto& members)
    {
        for (const auto& name : members.allMemberNames)
        {
            outMembers.insert(name);
        }
        for (const auto& name : members.accessorPropertyNames)
        {
            outMembers.insert(name);
        }
    };

    insertMembers(ruleIndex.ClassMembers(containerName));

    if (HasScopeQualifier(containerName))
    {
        insertMembers(ruleIndex.ClassMembers(LastScopeSegment(containerName)));
    }

    auto it = ruleIndex.qualifiedTypesByShortName.find(containerName);
    if (it != ruleIndex.qualifiedTypesByShortName.end())
    {
        for (const auto& qName : it->second)
        {
            insertMembers(ruleIndex.ClassMembers(qName));
        }
    }
}

/**
 * @brief Collects all members across an inheritance hierarchy.
 * @param[in] hierarchy Sequence of ancestor container names.
 * @param[in] ruleIndex Precalculated index of class and container members.
 * @param[out] outMembers Destination set receiving aggregated member names.
 */
static void CollectHierarchyMembers(std::span<const std::string_view> hierarchy, const RuleIndex& ruleIndex,
                                    MemberNameSet& outMembers)
{
    for (const auto ancestor : hierarchy)
    {
        CollectContainerMembers(ancestor, ruleIndex, outMembers);
    }
}

/**
 * @brief Resolves the symbol representing a mixin class.
 * @param[in] mixinName Identifier or scoped name of the mixin to look up.
 * @param[in] table Symbol table used for symbol lookup.
 * @param[out] fallback Storage for resolved fallback symbol if found by short name.
 * @return Pointer to resolved mixin symbol, or nullptr if not found.
 */
static const Symbol* FindMixinSymbol(std::string_view mixinName, const SymbolTable& table,
                                     std::optional<Symbol>& fallback)
{
    if (auto candidates = table.FindSymbolsPtr(mixinName))
    {
        for (const auto& c : *candidates)
        {
            if (c.type == SymbolType::Class && c.GetClass().modifiers.isMixin)
            {
                return &c;
            }
        }
    }

    const std::string_view shortName = LastScopeSegment(mixinName);
    auto shortCands = table.FindTypeSymbolsByShortName(shortName);
    for (const auto& c : shortCands)
    {
        if (c.type == SymbolType::Class && c.GetClass().modifiers.isMixin)
        {
            fallback = c;
            return &*fallback;
        }
    }
    return nullptr;
}

/**
 * @brief Locates the point range where a mixin is included in the host class.
 * @param[in] sym Host class symbol enclosing the mixin inclusion.
 * @param[in] mixinSym Resolved mixin symbol being included.
 * @param[in] mixinName Spelled identifier of the mixin inclusion.
 * @param[in] ctx Diagnostic context containing request node index and source.
 * @return Source range locating the mixin inclusion.
 */
static SourceRange FindHostMixinRange(const Symbol& sym, const Symbol& mixinSym, std::string_view mixinName,
                                      const DiagnosticContext& ctx)
{
    const uint32_t startL = sym.selectionRange.endLine > sym.selectionRange.startLine ||
                                    sym.selectionRange.endCharacter > sym.selectionRange.startCharacter
                                ? sym.selectionRange.startLine
                                : sym.startLine;
    const uint32_t startC = sym.selectionRange.endLine > sym.selectionRange.startLine ||
                                    sym.selectionRange.endCharacter > sym.selectionRange.startCharacter
                                ? sym.selectionRange.startCharacter
                                : sym.startCharacter;
    const uint32_t endL = sym.selectionRange.endLine > sym.selectionRange.startLine ||
                                  sym.selectionRange.endCharacter > sym.selectionRange.startCharacter
                              ? sym.selectionRange.endLine
                              : sym.endLine;
    const uint32_t endC = sym.selectionRange.endLine > sym.selectionRange.startLine ||
                                  sym.selectionRange.endCharacter > sym.selectionRange.startCharacter
                              ? sym.selectionRange.endCharacter
                              : sym.endCharacter;
    SourceRange range{startL, startC, endL, endC};
    const NodeIndex* hostIndex = ctx.request.GetNodeIndex();
    if (!hostIndex)
    {
        return range;
    }
    const MixinInclusionQuery query{{sym.startLine, sym.startCharacter}, mixinName, mixinSym.name};
    if (const auto span = FindMixinInclusionSpan(*hostIndex, ctx.request.sourceCode, query))
    {
        return SourceRange{span->startRow, span->startColumn, span->endRow, span->endColumn};
    }
    return range;
}

/**
 * @brief Collects direct members of a mixin symbol.
 * @param[in] mixinSym Mixin symbol whose members are collected.
 * @param[in] ruleIndex Precalculated index of class and container members.
 * @param[out] outMembers Destination set receiving mixin member names.
 */
static void CollectMixinSelfMembers(const Symbol& mixinSym, const RuleIndex& ruleIndex, MemberNameSet& outMembers)
{
    CollectContainerMembers(mixinSym.qualifiedName, ruleIndex, outMembers);
    if (mixinSym.name != mixinSym.qualifiedName)
    {
        CollectContainerMembers(mixinSym.name, ruleIndex, outMembers);
    }
}

/**
 * @brief Resolves the source text and node index a mixin body is checked against.
 * @param[in] mixinSym Mixin symbol whose declaring document is analysed.
 * @param[in,out] req Instantiation request; its file cache receives a parsed entry for other documents on first use.
 * @return Target borrowing the current document when the mixin lives there, otherwise the cached parse; an empty
 *         target when the other document cannot be read.
 */
static MixinAstTarget ResolveMixinAstTarget(const Symbol& mixinSym, MixinInstantiationCheckRequest& req)
{
    MixinAstTarget target;
    if (mixinSym.fileUri == req.ctx.request.fileUri)
    {
        target.source = req.ctx.request.sourceCode;
        target.nodeIndex = req.ctx.request.GetNodeIndex();
        return target;
    }

    auto it = req.fileCache.find(mixinSym.fileUri);
    if (it == req.fileCache.end())
    {
        MixinFileCacheEntry entry = mixinSym.fileUri == req.ctx.request.fileUri
                                        ? BuildMixinFileCacheEntryFromText(req.ctx.request.sourceCode)
                                        : LoadMixinFileCacheEntry(mixinSym.fileUri);
        if (entry.source.empty())
        {
            return target;
        }
        it = req.fileCache.emplace(mixinSym.fileUri, std::move(entry)).first;
    }
    target.source = it->second.source;
    target.nodeIndex = it->second.nodeIndex.get();
    return target;
}

/**
 * @brief Validates a single mixin instantiation against host class.
 * @param[in,out] req Request parameters including mixin name, host symbol, and file cache.
 */
static void CheckSingleMixinInstantiation(MixinInstantiationCheckRequest& req)
{
    std::optional<Symbol> fallbackMixin;
    const Symbol* mixinSym = FindMixinSymbol(req.mixinName, req.ctx.request.symbolTable, fallbackMixin);
    if (!mixinSym)
    {
        return;
    }

    MemberNameSet mixinSelfMembers;
    CollectMixinSelfMembers(*mixinSym, req.ctx.request.GetRuleIndex(), mixinSelfMembers);

    MixinAstTarget target = ResolveMixinAstTarget(*mixinSym, req);
    if (target.source.empty() || !target.nodeIndex)
    {
        return;
    }

    const MixinBodyFacts facts = BuildMixinBodyFacts(*target.nodeIndex, target.source,
                                                     IndexedSourcePoint{mixinSym->startLine, mixinSym->startCharacter});
    if (!facts.found)
    {
        return;
    }

    SourceRange hostRange = FindHostMixinRange(req.hostSym, *mixinSym, req.mixinName, req.ctx);
    MemberNameSet reportedMissingMembers;
    MixinCheckContext mctx{
        req.baseMembers.hostClassName, mixinSym,        hostRange, req.baseMembers.hostMembers, mixinSelfMembers,
        reportedMissingMembers,        target.nodeIndex};
    CheckMixinFunctions(facts, target.source, mctx, req.ctx);
}

/**
 * @brief Checks that internal statements of mixins instantiated in a host class resolve against the host.
 * @param[in] sym Host class symbol that includes the mixins.
 * @param[in] sig Host class signature listing the included mixin names.
 * @param[in] ctx Diagnostic context that receives the related diagnostics.
 * @param[in,out] sharedCache Optional cross-document cache of parsed mixin files; a local cache is used when null.
 */
void CheckMixinInstantiations(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx,
                              MixinFileCache* sharedCache)
{
    if (sig.modifiers.isMixin)
    {
        return;
    }

    const std::vector<std::string_view> includedMixins = CollectIncludedMixins(sig, ctx.request.symbolTable);
    if (includedMixins.empty())
    {
        return;
    }

    const std::string_view hostClassName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    const auto hostHierarchy = GetInheritedTypeHierarchy(hostClassName, ctx.request.symbolTable);
    std::vector<std::string_view> hierarchyViews;
    hierarchyViews.reserve(hostHierarchy.size());
    for (const auto& ancestor : hostHierarchy)
    {
        hierarchyViews.push_back(ancestor);
    }

    MixinHierarchyMembers baseMembers{hostClassName, {}};
    CollectHierarchyMembers(hierarchyViews, ctx.request.GetRuleIndex(), baseMembers.hostMembers);

    MixinFileCache localCache;
    MixinFileCache& effectiveCache = sharedCache ? *sharedCache : localCache;
    for (const auto mixinName : includedMixins)
    {
        MixinInstantiationCheckRequest req{mixinName, sym, baseMembers, ctx, effectiveCache};
        CheckSingleMixinInstantiation(req);
    }
}

} // namespace angel_lsp::analysis::rules
