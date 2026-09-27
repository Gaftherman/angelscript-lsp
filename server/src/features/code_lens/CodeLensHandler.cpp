#include "features/code_lens/CodeLensHandler.h"
#include "analysis/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <span>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Range key for deduplicating code lenses covering the same span.
 */
struct RangeKey
{
    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;

    bool operator==(const RangeKey& other) const noexcept
    {
        return startLine == other.startLine && startCharacter == other.startCharacter && endLine == other.endLine &&
               endCharacter == other.endCharacter;
    }
};

/**
 * @brief Hash functor for RangeKey.
 */
struct RangeKeyHash
{
    size_t operator()(const RangeKey& k) const noexcept
    {
        size_t h = std::hash<uint32_t>{}(k.startLine);
        h ^= std::hash<uint32_t>{}(k.startCharacter) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.endLine) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.endCharacter) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

/**
 * @brief Finds the name of the class enclosing the given document line.
 * @param[in] symbolTable Symbol table to search.
 * @param[in] uri File URI.
 * @param[in] line Document line index.
 * @return Enclosing class name, or empty string if not within a class.
 */
std::string GetEnclosingClassName(const analysis::SymbolTable& symbolTable, const std::string& uri, uint32_t line)
{
    std::string enclosingClass;
    symbolTable.ForEachSymbolInFile(
        uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if ((sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface) &&
                    sym.fileUri == uri)
                {
                    if (line >= sym.startLine && line <= sym.endLine)
                    {
                        enclosingClass = sym.name;
                    }
                }
            }
        });
    return enclosingClass;
}

using DeclRangeSet = ankerl::unordered_dense::set<std::tuple<std::string, uint32_t, uint32_t>>;

/**
 * @brief Criteria bundle for filtering and collecting symbol references across scopes.
 */
struct ReferenceCollectionCriteria
{
    const std::string& targetName;
    const std::vector<analysis::Symbol>& group;
    const ankerl::unordered_dense::set<std::string>& compatibleClasses;
    const DeclRangeSet& allDeclRanges;
    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    const analysis::SymbolTable& symbolTable;
    const CodeLensRequest& request;
};

/**
 * @brief Determines if a reference matches a known declaration or definition site.
 * @param[in] fileUri URI of document owning scope.
 * @param[in] ref Reference to check.
 * @param[in] scope Lexical scope of the reference.
 * @param[in] criteria Active search criteria.
 * @return True if reference represents a declaration or definition site.
 */
bool IsDeclarationOrDefinition(const std::string& fileUri, const analysis::LocalReference& ref,
                               const analysis::Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    if (criteria.allDeclRanges.contains({fileUri, ref.startLine, ref.startCharacter}))
    {
        return true;
    }
    for (const auto& sym : criteria.group)
    {
        if (fileUri == sym.fileUri && ref.startLine == sym.selectionRange.startLine &&
            ref.startCharacter == sym.selectionRange.startCharacter)
        {
            return true;
        }
    }
    for (const auto& def : scope->definitions)
    {
        if (def.name == criteria.targetName && def.startLine == ref.startLine &&
            def.startCharacter == ref.startCharacter)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks whether an enclosing class resides within the target symbol's inheritance hierarchy.
 * @param[in] encClass Enclosing class name.
 * @param[in] group Symbol group sharing declaration range.
 * @param[in] symbolTable Symbol table.
 * @return True if enclosing class is compatible with symbol group hierarchy.
 */
bool IsInTargetHierarchy(const std::string& encClass, const std::vector<analysis::Symbol>& group,
                         const analysis::SymbolTable& symbolTable)
{
    std::string cleanEnc = analysis::CleanBaseType(encClass);
    for (const auto& s : group)
    {
        if (s.containerName.empty())
        {
            continue;
        }
        std::string cleanDecl = analysis::CleanBaseType(s.containerName);
        if (cleanEnc == cleanDecl)
        {
            return true;
        }
        auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanEnc, symbolTable);
        for (const auto& ancestor : hierarchy)
        {
            if (analysis::CleanBaseType(ancestor) == cleanDecl)
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Verifies whether a non-member reference is a valid access to an inherited or enclosed symbol.
 * @param[in] fileUri URI of document owning scope.
 * @param[in] ref Reference to check.
 * @param[in] scope Lexical scope of reference.
 * @param[in] criteria Active search criteria.
 * @return True if non-member access is compatible.
 */
bool IsCompatibleNonMemberAccess(const std::string& fileUri, const analysis::LocalReference& ref,
                                 const analysis::Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    std::string encClass = GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine);
    if (encClass.empty() || !criteria.compatibleClasses.contains(encClass))
    {
        return false;
    }
    if (!IsInTargetHierarchy(encClass, criteria.group, criteria.symbolTable))
    {
        return false;
    }
    const analysis::LocalDefinition* localShadow = analysis::ResolveInScope(scope, criteria.targetName);
    return !(localShadow && (localShadow->kind == analysis::LocalDefinitionKind::Parameter ||
                             localShadow->kind == analysis::LocalDefinitionKind::Variable));
}

/**
 * @brief Resolves receiver type name from AST for member expression references in the current document.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] request Active CodeLens request.
 * @return Cleaned receiver type name, or empty string if not resolvable.
 */
/**
 * @brief Resolves the type of an object text identifier within local or global scope.
 * @param[in] oText Identifier text.
 * @param[in] scope Lexical scope.
 * @param[in] symbolTable Global symbol table.
 * @param[in] enclosingClass Enclosing class name if identifier is "this".
 * @return Cleaned type name, or empty string if unresolvable.
 */
std::string ResolveObjectTextType(const std::string& oText, const analysis::Scope* scope,
                                  const analysis::SymbolTable& symbolTable, const std::string& enclosingClass)
{
    if (oText == "this")
    {
        return enclosingClass;
    }
    const analysis::LocalDefinition* oDef = analysis::ResolveInScope(scope, oText);
    if (oDef && !oDef->typeName.empty())
    {
        return analysis::CleanBaseType(oDef->typeName);
    }
    auto gSyms = symbolTable.FindSymbols(oText);
    for (const auto& gs : gSyms)
    {
        if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(gs.GetVariable().typeName);
        }
    }
    return "";
}

/**
 * @brief Extracts the object expression text for a member expression from AST.
 * @param[in] request Active CodeLens request.
 * @param[in] ref Reference to check.
 * @return Object text or empty string.
 */
std::string GetMemberObjectText(const CodeLensRequest& request, const analysis::LocalReference& ref)
{
    TSNode rootNode = ts_tree_root_node(request.tree);
    TSPoint pt = {ref.startLine, ref.startCharacter};
    TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(refNode))
    {
        return "";
    }
    TSNode exprParent = ts_node_parent(refNode);
    if (ts_node_is_null(exprParent) || std::string_view(ts_node_type(exprParent)) != "member_expression")
    {
        return "";
    }
    TSNode objNode = parser::GetChildByField(exprParent, parser::fields::Object);
    if (ts_node_is_null(objNode))
    {
        return "";
    }
    uint32_t oStart = ts_node_start_byte(objNode);
    uint32_t oEnd = ts_node_end_byte(objNode);
    if (oStart >= request.sourceCode.size() || oEnd > request.sourceCode.size() || oStart >= oEnd)
    {
        return "";
    }
    return request.sourceCode.substr(oStart, oEnd - oStart);
}

/**
 * @brief Resolves receiver type name from AST for member expression references in the current document.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] request Active CodeLens request.
 * @return Cleaned receiver type name, or empty string if not resolvable.
 */
std::string ResolveReceiverFromAst(const std::string& fileUri, const analysis::LocalReference& ref,
                                   const analysis::Scope* scope, const CodeLensRequest& request)
{
    if (fileUri != request.uri || !request.tree || request.sourceCode.empty())
    {
        return "";
    }
    std::string oText = GetMemberObjectText(request, ref);
    if (oText.empty())
    {
        return "";
    }
    std::string enc = (oText == "this") ? GetEnclosingClassName(request.symbolTable, fileUri, ref.startLine) : "";
    return ResolveObjectTextType(oText, scope, request.symbolTable, enc);
}

/**
 * @brief Resolves receiver type name from scope symbols when AST lookup is unavailable.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] symbolTable Symbol table.
 * @return Cleaned receiver type name, or empty string if not resolvable.
 */
std::string ResolveReceiverFromScope(const std::string& fileUri, const analysis::LocalReference& ref,
                                     const analysis::Scope* scope, const analysis::SymbolTable& symbolTable)
{
    for (const auto& candRef : scope->references)
    {
        if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
        {
            std::string enc =
                (candRef.name == "this") ? GetEnclosingClassName(symbolTable, fileUri, ref.startLine) : "";
            std::string rType = ResolveObjectTextType(candRef.name, scope, symbolTable, enc);
            if (!rType.empty())
            {
                return rType;
            }
        }
    }
    return "";
}

/**
 * @brief Checks whether a member expression reference targets a compatible receiver class.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if receiver class is compatible.
 */
bool IsCompatibleMemberAccess(const std::string& fileUri, const analysis::LocalReference& ref,
                              const analysis::Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    std::string rType = ResolveReceiverFromAst(fileUri, ref, scope, criteria.request);
    if (rType.empty())
    {
        rType = ResolveReceiverFromScope(fileUri, ref, scope, criteria.symbolTable);
    }
    return !rType.empty() && criteria.compatibleClasses.contains(rType);
}

/**
 * @brief Validates if argument count matches the expected parameter constraints.
 * @param[in] ref Reference to check.
 * @param[in] criteria Active search criteria.
 * @return True if argument count is within valid bounds.
 */
bool MatchesCallArguments(const analysis::LocalReference& ref, const ReferenceCollectionCriteria& criteria)
{
    if (criteria.isFunction && ref.isCall)
    {
        return ref.argumentCount >= criteria.minArgs && ref.argumentCount <= criteria.maxArgs;
    }
    return true;
}

/**
 * @brief Checks whether a reference has valid accessibility and scope visibility.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if access is valid.
 */
bool IsValidAccess(const std::string& fileUri, const analysis::LocalReference& ref, const analysis::Scope* scope,
                   const ReferenceCollectionCriteria& criteria)
{
    if (!criteria.compatibleClasses.empty())
    {
        if (criteria.targetAccess == analysis::AccessModifier::Private ||
            criteria.targetAccess == analysis::AccessModifier::Protected)
        {
            std::string encClass = GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine);
            if (encClass.empty() || !criteria.compatibleClasses.contains(encClass))
            {
                return false;
            }
        }
        return ref.isMemberAccess ? IsCompatibleMemberAccess(fileUri, ref, scope, criteria)
                                  : IsCompatibleNonMemberAccess(fileUri, ref, scope, criteria);
    }

    if (ref.isMemberAccess)
    {
        return false;
    }
    const analysis::LocalDefinition* localShadow = analysis::ResolveInScope(scope, criteria.targetName);
    return !(localShadow && (localShadow->kind == analysis::LocalDefinitionKind::Parameter ||
                             localShadow->kind == analysis::LocalDefinitionKind::Variable));
}

/**
 * @brief Target item for batched reference counting across workspace scopes.
 */
struct SymbolLensTarget
{
    RangeKey key;
    std::string targetName;
    const std::vector<analysis::Symbol>& group;
    ankerl::unordered_dense::set<std::string> compatibleClasses;
    DeclRangeSet allDeclRanges;
    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenRefs;
};

/**
 * @brief Context bundle for batched reference counting across scope trees.
 */
struct BatchReferenceContext
{
    std::span<SymbolLensTarget> targets;
    const ankerl::unordered_dense::map<std::string, std::vector<size_t>>& targetsByName;
    const CodeLensRequest& request;
};

/**
 * @brief Evaluates scope references against indexed symbol targets.
 * @param[in] fileUri URI of document owning scope.
 * @param[in] scope Lexical scope to evaluate.
 * @param[in,out] ctx Batched reference context.
 */
void ProcessScopeReferencesBatch(const std::string& fileUri, const analysis::Scope* scope,
                                 const BatchReferenceContext& ctx)
{
    for (const auto& ref : scope->references)
    {
        auto it = ctx.targetsByName.find(ref.name);
        if (it == ctx.targetsByName.end())
        {
            continue;
        }

        const uint64_t pos = (static_cast<uint64_t>(ref.startLine) << 32) | ref.startCharacter;
        for (size_t targetIdx : it->second)
        {
            auto& target = ctx.targets[targetIdx];
            if (target.seenRefs.contains({fileUri, pos}))
            {
                continue;
            }
            ReferenceCollectionCriteria criteria{
                .targetName = target.targetName,
                .group = target.group,
                .compatibleClasses = target.compatibleClasses,
                .allDeclRanges = target.allDeclRanges,
                .targetAccess = target.targetAccess,
                .isFunction = target.isFunction,
                .minArgs = target.minArgs,
                .maxArgs = target.maxArgs,
                .symbolTable = ctx.request.symbolTable,
                .request = ctx.request,
            };
            if (IsDeclarationOrDefinition(fileUri, ref, scope, criteria))
            {
                continue;
            }
            if (!MatchesCallArguments(ref, criteria))
            {
                continue;
            }
            if (!IsValidAccess(fileUri, ref, scope, criteria))
            {
                continue;
            }
            target.seenRefs.insert({fileUri, pos});
        }
    }
}

/**
 * @brief Traverses a scope tree flatly to collect references for batched targets.
 * @param[in] fileUri URI of document owning scope tree.
 * @param[in] root Root scope of document.
 * @param[in,out] ctx Batched reference context.
 */
void CollectReferencesInScopeTreeBatch(const std::string& fileUri, const analysis::Scope* root,
                                      const BatchReferenceContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{root};
    while (!stack.empty())
    {
        const analysis::Scope* scope = stack.back();
        stack.pop_back();
        ProcessScopeReferencesBatch(fileUri, scope, ctx);
        for (const auto& child : scope->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Counts references across all documents in a single pass over the scope index.
 * @param[in,out] targets Vector of reference targets to update.
 * @param[in] targetsByName Mapping from symbol name to target indices.
 * @param[in] request Active CodeLens request.
 */
void BatchCountReferencesAcrossScopes(
    std::span<SymbolLensTarget> targets,
    const ankerl::unordered_dense::map<std::string, std::vector<size_t>>& targetsByName,
    const CodeLensRequest& request)
{
    BatchReferenceContext ctx{targets, targetsByName, request};
    request.scopeIndex.ForEachScopeTree(
        [&](const std::string& fileUri, const std::shared_ptr<const analysis::Scope>& root)
        {
            if (root)
            {
                CollectReferencesInScopeTreeBatch(fileUri, root.get(), ctx);
            }
        });
}

/**
 * @brief Constructs a command code lens with the given title and range.
 * @param[in] key Range covering the symbol.
 * @param[in] title Human-readable command title.
 * @return Configured CodeLens.
 */
lsp::CodeLens MakeCommandLens(const RangeKey& key, std::string title)
{
    lsp::CodeLens lens;
    lens.range =
        lsp::Range{lsp::Position{key.startLine, key.startCharacter}, lsp::Position{key.endLine, key.endCharacter}};
    lsp::Command cmd;
    cmd.title = std::move(title);
    cmd.command = "";
    lens.command = std::move(cmd);
    return lens;
}

/**
 * @brief Locates a mixin symbol definition in the symbol table.
 * @param[in] mixinName Qualified or bare mixin name.
 * @param[in] symbolTable Global symbol table.
 * @return Optional matching mixin Symbol.
 */
std::optional<analysis::Symbol> FindMixinSymbol(const std::string& mixinName, const analysis::SymbolTable& symbolTable)
{
    if (auto symsPtr = symbolTable.FindSymbolsPtr(mixinName))
    {
        for (const auto& c : *symsPtr)
        {
            if (c.type == analysis::SymbolType::Class && c.GetClass().modifiers.isMixin)
            {
                return c;
            }
        }
    }
    std::string shortName = analysis::LastScopeSegment(mixinName);
    for (const auto& c : symbolTable.FindTypeSymbolsByShortName(shortName))
    {
        if (c.type == analysis::SymbolType::Class && c.GetClass().modifiers.isMixin)
        {
            return c;
        }
    }
    return std::nullopt;
}

/**
 * @brief Handles virtual URI scheme requests by generating a jump-to-physical-source lens.
 * @param[in] request Active CodeLens request.
 * @return Optional vector of code lenses for the virtual URI.
 */
std::optional<std::vector<lsp::CodeLens>> HandleVirtualUri(const CodeLensRequest& request)
{
    if (!request.uri.starts_with("angelscript-virtual:") && !request.uri.starts_with("angelscript-virtual://"))
    {
        return std::nullopt;
    }

    std::string_view s = request.uri;
    static constexpr std::string_view kVirtualSchemeFull = "angelscript-virtual://";
    static constexpr std::string_view kVirtualSchemeShort = "angelscript-virtual:";

    if (s.starts_with(kVirtualSchemeFull))
    {
        s.remove_prefix(kVirtualSchemeFull.size());
    }
    else if (s.starts_with(kVirtualSchemeShort))
    {
        s.remove_prefix(kVirtualSchemeShort.size());
        while (!s.empty() && s.front() == '/')
        {
            s.remove_prefix(1);
        }
    }

    auto slashPos = s.find('/');
    std::string_view mixinPart = (slashPos != std::string_view::npos) ? s.substr(slashPos + 1) : s;
    if (mixinPart.ends_with(".as"))
    {
        mixinPart.remove_suffix(3);
    }
    std::string mixinName = utils::UrlDecode(mixinPart);

    auto mixinSym = FindMixinSymbol(mixinName, request.symbolTable);
    if (!mixinSym || mixinSym->fileUri.empty())
    {
        return std::nullopt;
    }

    std::string physicalPath = utils::UriToPath(mixinSym->fileUri);
    std::string filename = std::filesystem::path(physicalPath).filename().string();
    if (filename.empty())
    {
        filename = mixinSym->fileUri;
    }

    lsp::CodeLens lens;
    lens.range = lsp::Range{lsp::Position{0, 0}, lsp::Position{0, 0}};
    lsp::Command cmd;
    cmd.title = fmt::format("Jump to physical source in {}", filename);
    cmd.command = "angelscript.openPhysicalSource";

    lsp::json::Array args;
    lsp::json::Object argObj;
    argObj["fileUri"] = lsp::json::Value(std::string(mixinSym->fileUri));
    argObj["line"] = static_cast<lsp::json::Integer>(mixinSym->startLine);
    argObj["character"] = static_cast<lsp::json::Integer>(mixinSym->startCharacter);
    args.push_back(lsp::json::Value(std::move(argObj)));
    cmd.arguments = std::move(args);

    lens.command = std::move(cmd);
    return std::vector<lsp::CodeLens>{std::move(lens)};
}

using GroupMap = ankerl::unordered_dense::map<RangeKey, std::vector<analysis::Symbol>, RangeKeyHash>;

/**
 * @brief Grouped symbols and their sorted document order.
 */
struct SymbolGroups
{
    std::vector<RangeKey> rangeOrder;
    GroupMap groups;
};

/**
 * @brief Groups symbols in the request document by declaration range and sorts them top-down.
 * @param[in] request Active CodeLens request.
 * @return Structured SymbolGroups.
 */
SymbolGroups CollectSymbolGroups(const CodeLensRequest& request)
{
    SymbolGroups result;
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.fileUri != request.uri)
                {
                    continue;
                }

                const RangeKey key{sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter};
                auto [it, inserted] = result.groups.try_emplace(key, std::vector<analysis::Symbol>{});
                if (inserted)
                {
                    result.rangeOrder.push_back(key);
                }
                it->second.push_back(sym);
            }
        });

    std::sort(result.rangeOrder.begin(), result.rangeOrder.end(),
              [](const RangeKey& a, const RangeKey& b)
              {
                  if (a.startLine != b.startLine)
                  {
                      return a.startLine < b.startLine;
                  }
                  return a.startCharacter < b.startCharacter;
              });

    return result;
}

/**
 * @brief Selects the primary symbol from a group, preferring non-synthesized declarations.
 * @param[in] symGroup Group of symbols sharing a declaration range.
 * @return Primary symbol reference.
 */
const analysis::Symbol& SelectPrimarySymbol(const std::vector<analysis::Symbol>& symGroup)
{
    for (const auto& s : symGroup)
    {
        if (!s.isSynthesized)
        {
            return s;
        }
    }
    return symGroup.front();
}

/**
 * @brief Checks if a method belongs to an interface.
 * @param[in] sym Method symbol.
 * @param[in] symbolTable Global symbol table.
 * @return True if method belongs to an interface.
 */
bool IsInterfaceMethod(const analysis::Symbol& sym, const analysis::SymbolTable& symbolTable)
{
    if (sym.containerName.empty())
    {
        return false;
    }
    auto owners = symbolTable.FindSymbolsPtr(sym.containerName);
    if (!owners)
    {
        return false;
    }
    for (const auto& owner : *owners)
    {
        if (owner.type == analysis::SymbolType::Interface)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Counts class implementations of an interface method.
 * @param[in] sym Interface method symbol.
 * @param[in] symbolTable Global symbol table.
 * @return Implementation count.
 */
size_t CountInterfaceMethodImplementations(const analysis::Symbol& sym, const analysis::SymbolTable& symbolTable)
{
    size_t implCount = 0;
    symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& candidates)
        {
            for (const auto& cand : candidates)
            {
                if (cand.type != analysis::SymbolType::Function || cand.name != sym.name ||
                    (cand.fileUri == sym.fileUri && cand.containerName == sym.containerName) ||
                    cand.containerName.empty())
                {
                    continue;
                }
                auto candOwners = symbolTable.FindSymbolsPtr(cand.containerName);
                if (!candOwners)
                {
                    continue;
                }
                for (const auto& cOwner : *candOwners)
                {
                    if (cOwner.type == analysis::SymbolType::Class)
                    {
                        for (const auto& b : cOwner.GetClass().bases)
                        {
                            if (analysis::CleanBaseType(b) == sym.containerName &&
                                cand.GetFunction().parameters.size() == sym.GetFunction().parameters.size())
                            {
                                implCount++;
                            }
                        }
                    }
                }
            }
        });
    return implCount;
}

/**
 * @brief Counts classes that implement an interface.
 * @param[in] sym Interface symbol.
 * @param[in] symbolTable Global symbol table.
 * @return Implementation count.
 */
size_t CountInterfaceImplementations(const analysis::Symbol& sym, const analysis::SymbolTable& symbolTable)
{
    size_t implCount = 0;
    symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& candidates)
        {
            for (const auto& cand : candidates)
            {
                if (cand.type == analysis::SymbolType::Class)
                {
                    for (const auto& b : cand.GetClass().bases)
                    {
                        if (analysis::CleanBaseType(b) == sym.name)
                        {
                            implCount++;
                            break;
                        }
                    }
                }
            }
        });
    return implCount;
}

using MixinInclusionMap = ankerl::unordered_dense::map<std::string, std::vector<std::string>>;

/**
 * @brief Context bundling mixin inclusion map and symbol table for compatible class resolution.
 */
struct CompatibleClassContext
{
    const MixinInclusionMap& mixinMap;
    const analysis::SymbolTable& symbolTable;
};

/**
 * @brief Precomputes mapping from included mixin names to including classes and their derived classes.
 * @param[in] symbolTable Global symbol table.
 * @return Map of mixin name to all compatible host and derived class names.
 */
MixinInclusionMap BuildMixinInclusionMap(const analysis::SymbolTable& symbolTable)
{
    MixinInclusionMap map;
    symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& cand : symbols)
            {
                if (cand.type == analysis::SymbolType::Class &&
                    std::holds_alternative<analysis::ClassSignature>(cand.signature))
                {
                    const auto& cls = cand.GetClass();
                    for (const auto& m : cls.includedMixins)
                    {
                        auto& list = map[m];
                        list.push_back(cand.name);
                        if (!cand.qualifiedName.empty() && cand.qualifiedName != cand.name)
                        {
                            list.push_back(cand.qualifiedName);
                        }
                        auto candDerived = analysis::GetDerivedClasses(cand.name, symbolTable);
                        for (const auto& rel : candDerived)
                        {
                            list.push_back(rel);
                        }
                    }
                }
            }
        });
    return map;
}

/**
 * @brief Collects all classes compatible with member symbol access across mixins and hierarchies.
 * @param[in] symGroup Symbol group sharing declaration range.
 * @param[in] sym Primary symbol.
 * @param[in] targetAccess Access modifier of the member.
 * @param[in] ctx Context bundling mixin map and symbol table.
 * @return Set of compatible class names.
 */
ankerl::unordered_dense::set<std::string> CollectCompatibleClasses(const std::vector<analysis::Symbol>& symGroup,
                                                                   const analysis::Symbol& sym,
                                                                   analysis::AccessModifier targetAccess,
                                                                   const CompatibleClassContext& ctx)
{
    ankerl::unordered_dense::set<std::string> compatibleClasses;
    for (const auto& s : symGroup)
    {
        if (!s.containerName.empty())
        {
            compatibleClasses.insert(s.containerName);
            auto lastColon = s.containerName.rfind("::");
            if (lastColon != std::string::npos)
            {
                compatibleClasses.insert(s.containerName.substr(lastColon + 2));
            }
            auto comp = analysis::GetCompatibleMemberClasses(s.containerName, sym.name, targetAccess, ctx.symbolTable);
            for (const auto& c : comp)
            {
                compatibleClasses.insert(c);
            }
        }
    }

    if (!compatibleClasses.empty() && targetAccess != analysis::AccessModifier::Private)
    {
        std::vector<std::string> initial(compatibleClasses.begin(), compatibleClasses.end());
        for (const auto& c : initial)
        {
            auto it = ctx.mixinMap.find(c);
            if (it != ctx.mixinMap.end())
            {
                for (const auto& candClass : it->second)
                {
                    compatibleClasses.insert(candClass);
                }
            }
        }
    }
    return compatibleClasses;
}

/**
 * @brief Adds declaration line and character coordinates of symbols to a range set.
 * @param[in] syms List of symbols.
 * @param[in,out] declRanges Output set of declaration coordinates.
 */
void AddSymbolDeclRanges(const std::vector<analysis::Symbol>& syms, DeclRangeSet& declRanges)
{
    for (const auto& s : syms)
    {
        if (s.type != analysis::SymbolType::CallReference)
        {
            uint32_t sL = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0)
                              ? s.selectionRange.startLine
                              : s.startLine;
            uint32_t sC = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0)
                              ? s.selectionRange.startCharacter
                              : s.startCharacter;
            declRanges.insert({s.fileUri, sL, sC});
        }
    }
}

/**
 * @brief Collects all declaration coordinates for a symbol and its compatible class variants.
 * @param[in] symName Member symbol name.
 * @param[in] symGroup Symbol group sharing declaration range.
 * @param[in] compatibleClasses Set of compatible classes.
 * @param[in] symbolTable Global symbol table.
 * @return Populated set of declaration coordinate tuples.
 */
DeclRangeSet CollectAllDeclRanges(const std::string& symName, const std::vector<analysis::Symbol>& symGroup,
                                  const ankerl::unordered_dense::set<std::string>& compatibleClasses,
                                  const analysis::SymbolTable& symbolTable)
{
    DeclRangeSet allDeclRanges;
    AddSymbolDeclRanges(symbolTable.FindSymbols(symName), allDeclRanges);
    for (const auto& c : compatibleClasses)
    {
        AddSymbolDeclRanges(symbolTable.FindSymbols(c + "::" + symName), allDeclRanges);
    }
    AddSymbolDeclRanges(symGroup, allDeclRanges);
    return allDeclRanges;
}

/**
 * @brief Searches base class list for a mixin usage range.
 * @param[in] hostNode Class declaration AST node.
 * @param[in] mixinName Target mixin name.
 * @param[in] sourceCode Source text.
 * @return Optional matching Range.
 */
std::optional<lsp::Range> FindMixinInBaseList(TSNode hostNode, const std::string& mixinName,
                                              const std::string& sourceCode)
{
    uint32_t childCount = ts_node_child_count(hostNode);
    for (uint32_t c = 0; c < childCount; ++c)
    {
        TSNode child = ts_node_child(hostNode, c);
        if (std::string_view(ts_node_type(child)) == "base_class_list")
        {
            uint32_t bCount = ts_node_named_child_count(child);
            for (uint32_t b = 0; b < bCount; ++b)
            {
                TSNode baseChild = ts_node_named_child(child, b);
                std::string bText = std::string(parser::AngelScriptParser::GetNodeText(baseChild, sourceCode));
                if (analysis::CleanBaseType(bText) == mixinName || bText == mixinName ||
                    bText.ends_with("::" + mixinName))
                {
                    TSPoint sp = ts_node_start_point(baseChild);
                    TSPoint ep = ts_node_end_point(baseChild);
                    return lsp::Range{lsp::Position{sp.row, sp.column}, lsp::Position{ep.row, ep.column}};
                }
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Searches class body for a mixin inclusion statement range.
 * @param[in] hostNode Class declaration AST node.
 * @param[in] mixinName Target mixin name.
 * @param[in] sourceCode Source text.
 * @return Optional matching Range.
 */
std::optional<lsp::Range> FindMixinInBody(TSNode hostNode, const std::string& mixinName, const std::string& sourceCode)
{
    TSNode bodyNode = parser::GetChildByField(hostNode, parser::fields::Body);
    if (!ts_node_is_null(bodyNode))
    {
        uint32_t mCount = ts_node_child_count(bodyNode);
        for (uint32_t m = 0; m < mCount; ++m)
        {
            TSNode mNode = ts_node_child(bodyNode, m);
            std::string mText = std::string(parser::AngelScriptParser::GetNodeText(mNode, sourceCode));
            if (mText.find(mixinName) != std::string::npos)
            {
                TSPoint sp = ts_node_start_point(mNode);
                TSPoint ep = ts_node_end_point(mNode);
                return lsp::Range{lsp::Position{sp.row, sp.column}, lsp::Position{ep.row, ep.column}};
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Locates the AST range corresponding to a mixin inclusion within a host class.
 * @param[in] sym Host class symbol.
 * @param[in] mixinName Included mixin name.
 * @param[in] request Active CodeLens request.
 * @return Source range of the mixin declaration or fallback to class header.
 */
lsp::Range FindMixinRangeInAst(const analysis::Symbol& sym, const std::string& mixinName,
                               const CodeLensRequest& request)
{
    const lsp::Range fallbackRange{lsp::Position{sym.startLine, sym.startCharacter},
                                   lsp::Position{sym.startLine, sym.endCharacter}};

    if (!request.tree || request.sourceCode.empty())
    {
        return fallbackRange;
    }

    TSNode hostRoot = ts_tree_root_node(request.tree);
    TSPoint hostPt = {sym.startLine, sym.startCharacter};
    TSNode hostNode = ts_node_descendant_for_point_range(hostRoot, hostPt, hostPt);
    while (!ts_node_is_null(hostNode) && std::string_view(ts_node_type(hostNode)) != "class_declaration")
    {
        hostNode = ts_node_parent(hostNode);
    }
    if (ts_node_is_null(hostNode))
    {
        return fallbackRange;
    }

    if (auto baseRange = FindMixinInBaseList(hostNode, mixinName, request.sourceCode))
    {
        return *baseRange;
    }
    if (auto bodyRange = FindMixinInBody(hostNode, mixinName, request.sourceCode))
    {
        return *bodyRange;
    }

    return fallbackRange;
}

/**
 * @brief Appends CodeLens entries for mixin expansions included by a host class.
 * @param[in] sym Host class symbol.
 * @param[in] request Active CodeLens request.
 * @param[in,out] lenses Output vector of CodeLens items.
 */
void AppendMixinExpansionLenses(const analysis::Symbol& sym, const CodeLensRequest& request,
                                std::vector<lsp::CodeLens>& lenses)
{
    if (!std::holds_alternative<analysis::ClassSignature>(sym.signature))
    {
        return;
    }
    const auto& sig = sym.GetClass();
    if (sig.modifiers.isMixin)
    {
        return;
    }

    std::vector<std::string> includedMixins = sig.includedMixins;
    for (const auto& b : sig.bases)
    {
        std::string clean = analysis::CleanBaseType(b);
        if (!clean.empty() && analysis::IsMixinClass(clean, request.symbolTable))
        {
            if (std::find(includedMixins.begin(), includedMixins.end(), clean) == includedMixins.end())
            {
                includedMixins.push_back(clean);
            }
        }
    }

    for (const auto& mixinName : includedMixins)
    {
        lsp::Range range = FindMixinRangeInAst(sym, mixinName, request);

        lsp::CodeLens mixinLens;
        mixinLens.range = range;
        lsp::Command mixinCmd;
        mixinCmd.title = fmt::format("View Mixin Expansion: {}", mixinName);
        mixinCmd.command = "angelscript.viewMixinExpansion";

        lsp::json::Array args;
        lsp::json::Object argObj;
        argObj["hostClass"] = lsp::json::Value(std::string(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName));
        argObj["mixinName"] = lsp::json::Value(std::string(mixinName));
        argObj["hostUri"] = lsp::json::Value(std::string(request.uri));
        argObj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.start.line));
        argObj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.start.character));

        if (auto symsPtr = request.symbolTable.FindSymbolsPtr(mixinName))
        {
            for (const auto& s : *symsPtr)
            {
                if (s.type == analysis::SymbolType::Class && s.GetClass().modifiers.isMixin)
                {
                    argObj["targetUri"] = lsp::json::Value(std::string(s.fileUri));
                    argObj["targetLine"] = lsp::json::Value(static_cast<lsp::json::Integer>(s.startLine));
                    argObj["targetCharacter"] = lsp::json::Value(static_cast<lsp::json::Integer>(s.startCharacter));
                    break;
                }
            }
        }

        args.push_back(lsp::json::Value(std::move(argObj)));
        mixinCmd.arguments = std::move(args);
        mixinLens.command = std::move(mixinCmd);
        lenses.push_back(std::move(mixinLens));
    }
}

/**
 * @brief Creates a reference target for a function or variable symbol.
 * @param[in] key Range covering symbol declaration.
 * @param[in] sym Primary symbol.
 * @param[in] symGroup Symbol group sharing declaration range.
 * @param[in] ctx Context bundling mixin map and symbol table.
 * @return Constructed SymbolLensTarget.
 */
SymbolLensTarget CreateFunctionOrVariableTarget(const RangeKey& key, const analysis::Symbol& sym,
                                                const std::vector<analysis::Symbol>& symGroup,
                                                const CompatibleClassContext& ctx)
{
    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    if (std::holds_alternative<analysis::FunctionSignature>(sym.signature))
    {
        const auto& fn = sym.GetFunction();
        targetAccess = fn.modifiers.access;
        isFunction = true;
        maxArgs = fn.parameters.size();
        for (const auto& p : fn.parameters)
        {
            if (p.defaultValue.empty())
            {
                minArgs++;
            }
        }
    }
    else if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
    {
        targetAccess = sym.GetVariable().modifiers.access;
    }

    auto compatibleClasses = CollectCompatibleClasses(symGroup, sym, targetAccess, ctx);
    auto allDeclRanges = CollectAllDeclRanges(sym.name, symGroup, compatibleClasses, ctx.symbolTable);

    return SymbolLensTarget{
        .key = key,
        .targetName = sym.name,
        .group = symGroup,
        .compatibleClasses = std::move(compatibleClasses),
        .allDeclRanges = std::move(allDeclRanges),
        .targetAccess = targetAccess,
        .isFunction = isFunction,
        .minArgs = minArgs,
        .maxArgs = maxArgs,
        .seenRefs = {},
    };
}

/**
 * @brief Creates a reference target for a class symbol.
 * @param[in] key Range covering class declaration.
 * @param[in] sym Class symbol.
 * @param[in] symGroup Symbol group sharing declaration range.
 * @return Constructed SymbolLensTarget.
 */
SymbolLensTarget CreateClassTarget(const RangeKey& key, const analysis::Symbol& sym,
                                   const std::vector<analysis::Symbol>& symGroup)
{
    DeclRangeSet allDeclRanges;
    AddSymbolDeclRanges(symGroup, allDeclRanges);

    return SymbolLensTarget{
        .key = key,
        .targetName = sym.name,
        .group = symGroup,
        .compatibleClasses = {},
        .allDeclRanges = std::move(allDeclRanges),
        .targetAccess = analysis::AccessModifier::Public,
        .isFunction = false,
        .minArgs = 0,
        .maxArgs = 0,
        .seenRefs = {},
    };
}

using KeyTargetMap = ankerl::unordered_dense::map<RangeKey, size_t, RangeKeyHash>;

/**
 * @brief Registers reference targets for symbols that require reference counts.
 * @param[in] symGroups Symbol groups in the active document.
 * @param[in] ctx Context bundling mixin map and symbol table.
 * @param[out] targets List of collected reference targets.
 * @param[out] keyToTarget Map from RangeKey to target index.
 */
void CollectReferenceTargets(const SymbolGroups& symGroups, const CompatibleClassContext& ctx,
                             std::vector<SymbolLensTarget>& targets, KeyTargetMap& keyToTarget)
{
    for (const auto& key : symGroups.rangeOrder)
    {
        auto it = symGroups.groups.find(key);
        if (it == symGroups.groups.end() || it->second.empty())
        {
            continue;
        }

        const auto& symGroup = it->second;
        const auto& sym = SelectPrimarySymbol(symGroup);

        if (sym.type == analysis::SymbolType::Function)
        {
            if (IsInterfaceMethod(sym, ctx.symbolTable))
            {
                continue;
            }
            keyToTarget[key] = targets.size();
            targets.push_back(CreateFunctionOrVariableTarget(key, sym, symGroup, ctx));
        }
        else if (sym.type == analysis::SymbolType::Variable)
        {
            keyToTarget[key] = targets.size();
            targets.push_back(CreateFunctionOrVariableTarget(key, sym, symGroup, ctx));
        }
        else if (sym.type == analysis::SymbolType::Class)
        {
            keyToTarget[key] = targets.size();
            targets.push_back(CreateClassTarget(key, sym, symGroup));
        }
    }
}

/**
 * @brief Context bundle holding assembled targets and the active request.
 */
struct AssembledLensContext
{
    const std::vector<SymbolLensTarget>& targets;
    const KeyTargetMap& keyToTarget;
    const CodeLensRequest& request;
};

/**
 * @brief Appends CodeLens for an individual symbol group.
 * @param[in] key Declaration range key.
 * @param[in] symGroup Symbols sharing the declaration range.
 * @param[in] ctx Assembled CodeLens context.
 * @param[in,out] lenses Output list of code lenses.
 */
void AppendLensesForGroup(const RangeKey& key, const std::vector<analysis::Symbol>& symGroup,
                          const AssembledLensContext& ctx, std::vector<lsp::CodeLens>& lenses)
{
    const auto& sym = SelectPrimarySymbol(symGroup);

    if (sym.type == analysis::SymbolType::Function)
    {
        if (IsInterfaceMethod(sym, ctx.request.symbolTable))
        {
            size_t implCount = CountInterfaceMethodImplementations(sym, ctx.request.symbolTable);
            std::string title = std::to_string(implCount) + (implCount == 1 ? " implementation" : " implementations");
            lenses.push_back(MakeCommandLens(key, std::move(title)));
            return;
        }

        auto it = ctx.keyToTarget.find(key);
        if (it != ctx.keyToTarget.end())
        {
            size_t refCount = ctx.targets[it->second].seenRefs.size();
            std::string title = std::to_string(refCount) + (refCount == 1 ? " reference" : " references");
            lenses.push_back(MakeCommandLens(key, std::move(title)));
        }
    }
    else if (sym.type == analysis::SymbolType::Interface)
    {
        size_t implCount = CountInterfaceImplementations(sym, ctx.request.symbolTable);
        std::string title = std::to_string(implCount) + (implCount == 1 ? " implementation" : " implementations");
        lenses.push_back(MakeCommandLens(key, std::move(title)));
    }
    else if (sym.type == analysis::SymbolType::Class)
    {
        auto it = ctx.keyToTarget.find(key);
        if (it != ctx.keyToTarget.end())
        {
            size_t refCount = ctx.targets[it->second].seenRefs.size();
            std::string title = std::to_string(refCount) + (refCount == 1 ? " reference" : " references");
            lenses.push_back(MakeCommandLens(key, std::move(title)));
        }
        AppendMixinExpansionLenses(sym, ctx.request, lenses);
    }
    else if (sym.type == analysis::SymbolType::Variable)
    {
        auto it = ctx.keyToTarget.find(key);
        if (it != ctx.keyToTarget.end())
        {
            size_t refCount = ctx.targets[it->second].seenRefs.size();
            std::string title = std::to_string(refCount) + (refCount == 1 ? " reference" : " references");
            lenses.push_back(MakeCommandLens(key, std::move(title)));
        }
    }
}

/**
 * @brief Constructs code lenses for each symbol group in document order.
 * @param[in] symGroups Collected and grouped symbols.
 * @param[in] request Active CodeLens request.
 * @return Populated vector of code lenses.
 */
std::vector<lsp::CodeLens> BuildSymbolGroupLenses(const SymbolGroups& symGroups, const CodeLensRequest& request)
{
    CompatibleClassContext classCtx{BuildMixinInclusionMap(request.symbolTable), request.symbolTable};

    std::vector<SymbolLensTarget> targets;
    KeyTargetMap keyToTarget;
    CollectReferenceTargets(symGroups, classCtx, targets, keyToTarget);

    if (!targets.empty())
    {
        ankerl::unordered_dense::map<std::string, std::vector<size_t>> targetsByName;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            targetsByName[targets[i].targetName].push_back(i);
        }
        BatchCountReferencesAcrossScopes(targets, targetsByName, request);
    }

    std::vector<lsp::CodeLens> lenses;
    AssembledLensContext assembledCtx{targets, keyToTarget, request};
    for (const auto& key : symGroups.rangeOrder)
    {
        auto it = symGroups.groups.find(key);
        if (it != symGroups.groups.end() && !it->second.empty())
        {
            AppendLensesForGroup(key, it->second, assembledCtx, lenses);
        }
    }
    return lenses;
}

} // namespace

std::optional<std::vector<lsp::CodeLens>> GetCodeLenses(const CodeLensRequest& request)
{
    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[CodeLens] Computing code lenses for URI: {}", request.uri));
    }

    if (auto virtualLenses = HandleVirtualUri(request))
    {
        return virtualLenses;
    }

    if (request.sourceCode.empty() ||
        angel_lsp::utils::IsPredefinedFile(request.uri, request.predefinedExtension))
    {
        return std::nullopt;
    }

    auto symGroups = CollectSymbolGroups(request);
    if (symGroups.rangeOrder.empty())
    {
        return std::nullopt;
    }

    auto lenses = BuildSymbolGroupLenses(symGroups, request);
    if (lenses.empty())
    {
        return std::nullopt;
    }

    if (request.logger && request.logger->IsTraceEnabled())
    {
        request.logger->LogTrace(
            fmt::format("[CodeLens] Generated {} code lenses for URI: {}", lenses.size(), request.uri));
    }

    return lenses;
}

std::optional<lsp::CodeLens> ResolveCodeLens(const CodeLensResolveRequest& request)
{
    if (request.logger && request.logger->IsTraceEnabled())
    {
        request.logger->LogTrace("[CodeLens] Resolving code lens");
    }
    return request.codeLens;
}
} // namespace angel_lsp::features
