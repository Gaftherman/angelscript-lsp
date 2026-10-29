#include "features/code_lens/CodeLensHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/UnusedSymbolChecker.h"
#include "config/EngineRuleConfig.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "utils/Constants.h"
#include "utils/LspLogger.h"
#include "utils/ScopedNameUtils.h"
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
 * @brief Enclosing class span data.
 */
struct EnclosingClassSpan
{
    uint32_t startLine = 0;
    uint32_t endLine = 0;
    std::string className;
};

using EnclosingClassCache = ankerl::unordered_dense::map<std::string, std::vector<EnclosingClassSpan>>;

/**
 * @brief Finds the name of the class enclosing the given document line, with optional caching.
 * @param[in] symbolTable Symbol table to search.
 * @param[in] uri File URI.
 * @param[in] line Document line index.
 * @param[in,out] cache Optional per-file class span cache.
 * @return Enclosing class name, or empty string if not within a class.
 */
std::string GetEnclosingClassName(const analysis::SymbolTable& symbolTable, const std::string& uri, uint32_t line,
                                  EnclosingClassCache* cache = nullptr)
{
    if (cache)
    {
        auto it = cache->find(uri);
        if (it == cache->end())
        {
            std::vector<EnclosingClassSpan> spans;
            symbolTable.ForEachSymbolInFile(
                uri,
                [&]([[maybe_unused]] const std::string& name, const std::vector<analysis::Symbol>& syms)
                {
                    for (const auto& s : syms)
                    {
                        if ((s.type == analysis::SymbolType::Class || s.type == analysis::SymbolType::Interface) &&
                            s.fileUri == uri)
                        {
                            spans.push_back(
                                {s.startLine, s.endLine, s.qualifiedName.empty() ? s.name : s.qualifiedName});
                        }
                    }
                });
            it = cache->emplace(uri, std::move(spans)).first;
        }
        uint32_t bestSpan = UINT32_MAX;
        std::string bestClass;
        for (const auto& cs : it->second)
        {
            if (line >= cs.startLine && line <= cs.endLine)
            {
                uint32_t span = cs.endLine - cs.startLine;
                if (span <= bestSpan)
                {
                    bestSpan = span;
                    bestClass = cs.className;
                }
            }
        }
        return bestClass;
    }
    return symbolTable.FindEnclosingClassName(uri, line);
}

using DeclRangeSet = ankerl::unordered_dense::set<std::tuple<std::string, uint32_t, uint32_t>>;

/**
 * @brief Checks if a container symbol is a class or interface rather than a namespace.
 * @param[in] container Name of the container.
 * @param[in] symbolTable Global symbol table.
 * @return True if container is a class or interface.
 */
bool IsContainerClass(std::string_view container, const analysis::SymbolTable& symbolTable)
{
    auto syms = symbolTable.FindSymbolsPtr(container);
    if (!syms)
    {
        return false;
    }
    for (const auto& s : *syms)
    {
        if (s.type == analysis::SymbolType::Class || s.type == analysis::SymbolType::Interface)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks whether a reference at given line in fileUri is inside a named namespace.
 * @param[in] fileUri Document URI.
 * @param[in] line Reference line.
 * @param[in] nsName Namespace name.
 * @param[in] symbolTable Global symbol table.
 * @return True if inside namespace block.
 */
bool IsInsideNamespace(const std::string& fileUri, uint32_t line, const std::string& nsName,
                       const analysis::SymbolTable& symbolTable, EnclosingClassCache* classCache = nullptr)
{
    if (auto syms = symbolTable.FindSymbolsPtr(nsName))
    {
        for (const auto& s : *syms)
        {
            if (s.type == analysis::SymbolType::Namespace && s.fileUri == fileUri)
            {
                if (line >= s.startLine && line <= s.endLine)
                {
                    return true;
                }
            }
        }
    }
    const std::string encClass = GetEnclosingClassName(symbolTable, fileUri, line, classCache);
    if (!encClass.empty())
    {
        if (analysis::IsSameOrNestedNamespace(encClass, nsName))
        {
            return true;
        }
        if (auto clsSyms = symbolTable.FindSymbolsPtr(encClass))
        {
            for (const auto& cs : *clsSyms)
            {
                if (analysis::IsSameOrNestedNamespace(cs.containerName, nsName))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Criteria bundle for filtering and collecting symbol references across scopes.
 */
struct ReferenceCollectionCriteria
{
    std::string targetName;
    const std::vector<analysis::Symbol>& group;
    const ankerl::unordered_dense::set<std::string>& compatibleClasses;
    const DeclRangeSet& allDeclRanges;
    std::string targetNamespace;
    std::string targetEnumContainer;
    bool isEnumConstant = false;
    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    const analysis::SymbolTable& symbolTable;
    const CodeLensRequest& request;
    EnclosingClassCache* classCache = nullptr;
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
    for (const auto* s = scope; s != nullptr; s = s->parent)
    {
        for (const auto& def : s->definitions)
        {
            if (def.name == criteria.targetName && def.startLine == ref.startLine &&
                def.startCharacter == ref.startCharacter)
            {
                return true;
            }
        }
    }
    return false;
}

using analysis::MatchesDeclOrScope;

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
        if (MatchesDeclOrScope(cleanEnc, cleanDecl))
        {
            return true;
        }
        auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanEnc, symbolTable);
        for (const auto& ancestor : hierarchy)
        {
            if (MatchesDeclOrScope(analysis::CleanBaseType(ancestor), cleanDecl))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Checks if a local definition shadows the target symbol within the given scope.
 * @param[in] scope Lexical scope.
 * @param[in] name Reference identifier name.
 * @param[in] fileUri Document URI.
 * @param[in] criteria Active search criteria.
 * @return True if a local variable or parameter shadows the target symbol.
 */
bool IsLocalShadowingTarget(const analysis::Scope* scope, std::string_view name, const std::string& fileUri,
                            const ReferenceCollectionCriteria& criteria)
{
    const analysis::LocalDefinition* localShadow = analysis::ResolveInScope(scope, name);
    if (!localShadow)
    {
        return false;
    }
    if (localShadow->kind != analysis::LocalDefinitionKind::Parameter &&
        localShadow->kind != analysis::LocalDefinitionKind::Variable)
    {
        return false;
    }
    if (criteria.allDeclRanges.contains({fileUri, localShadow->startLine, localShadow->startCharacter}))
    {
        return false;
    }
    for (const auto& sym : criteria.group)
    {
        if (fileUri == sym.fileUri && localShadow->startLine == sym.selectionRange.startLine &&
            localShadow->startCharacter == sym.selectionRange.startCharacter)
        {
            return false;
        }
    }
    return true;
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
    std::string encClass = GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine, criteria.classCache);
    if (encClass.empty() || (!criteria.compatibleClasses.contains(encClass) &&
                             !criteria.compatibleClasses.contains(std::string(analysis::LastScopeSegment(encClass)))))
    {
        return false;
    }
    if (!IsInTargetHierarchy(encClass, criteria.group, criteria.symbolTable))
    {
        return false;
    }
    return !IsLocalShadowingTarget(scope, criteria.targetName, fileUri, criteria);
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
std::string ResolveObjectTextType(std::string_view oText, const analysis::Scope* scope,
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
    if (ref.receiverStartByte >= ref.receiverEndByte || ref.receiverEndByte > request.sourceCode.size())
    {
        return "";
    }
    return std::string(std::string_view(request.sourceCode)
                           .substr(ref.receiverStartByte, ref.receiverEndByte - ref.receiverStartByte));
}

/**
 * @brief Attempts fast-path receiver type resolution using pre-indexed slice bounds.
 * @param[in] ref Reference containing receiver byte slice.
 * @param[in] criteria Active search criteria.
 * @param[in] scope Lexical scope.
 * @param[in] encClass Enclosing class name if applicable.
 * @return Resolved receiver type name, or empty string.
 */
std::string TryResolveReceiverFastPath(const analysis::LocalReference& ref, const ReferenceCollectionCriteria& criteria,
                                       const analysis::Scope* scope, const std::string& encClass)
{
    if (ref.receiverStartByte < ref.receiverEndByte && ref.receiverEndByte <= criteria.request.sourceCode.size() &&
        !ref.isScopedIdentifier)
    {
        std::string_view oText = std::string_view(criteria.request.sourceCode)
                                     .substr(ref.receiverStartByte, ref.receiverEndByte - ref.receiverStartByte);
        std::string enc = (oText == "this") ? encClass : "";
        return ResolveObjectTextType(oText, scope, criteria.symbolTable, enc);
    }
    return "";
}

/**
 * @brief Resolves receiver type name from AST member access object node.
 * @param[in] objNode Tree-Sitter AST object node.
 * @param[in] criteria Active search criteria.
 * @param[in] scope Lexical scope.
 * @param[in] encClass Enclosing class name if applicable.
 * @return Resolved receiver type name, or empty string.
 */
std::string TryResolveFromObjNode(TSNode objNode, const ReferenceCollectionCriteria& criteria,
                                  const analysis::Scope* scope, const std::string& encClass)
{
    std::string resolved = analysis::ResolveReceiverType(objNode, criteria.request.sourceCode, criteria.symbolTable,
                                                         {scope, encClass, criteria.request.uri});
    if (!resolved.empty())
    {
        return analysis::CleanBaseType(resolved);
    }
    uint32_t oStart = ts_node_start_byte(objNode);
    uint32_t oEnd = ts_node_end_byte(objNode);
    if (oStart >= criteria.request.sourceCode.size() || oEnd > criteria.request.sourceCode.size() || oStart >= oEnd)
    {
        return "";
    }
    std::string_view oText = std::string_view(criteria.request.sourceCode).substr(oStart, oEnd - oStart);
    std::string enc = (oText == "this") ? encClass : "";
    return ResolveObjectTextType(oText, scope, criteria.symbolTable, enc);
}

/**
 * @brief Resolves receiver type name from AST for member expression references in the current document.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return Cleaned receiver type name, or empty string if not resolvable.
 */
std::string ResolveReceiverFromAst(const std::string& fileUri, const analysis::LocalReference& ref,
                                   const analysis::Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    if (fileUri != criteria.request.uri || !criteria.request.tree || criteria.request.sourceCode.empty())
    {
        return "";
    }
    std::string encClass = GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine, criteria.classCache);
    std::string fastType = TryResolveReceiverFastPath(ref, criteria, scope, encClass);
    if (!fastType.empty())
    {
        return fastType;
    }
    TSNode rootNode = ts_tree_root_node(criteria.request.tree);
    TSPoint pt = {ref.startLine, ref.startCharacter};
    TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(refNode))
    {
        return "";
    }
    TSNode exprParent = parser::GetParent(refNode);
    const auto& syms = parser::GetGrammarSymbols();
    if (ts_node_is_null(exprParent) || ts_node_symbol(exprParent) != syms.symMemberAccess)
    {
        return "";
    }
    TSNode objNode = parser::GetChildByField(exprParent, parser::GetGrammarSymbols().fieldObject);
    if (ts_node_is_null(objNode))
    {
        return "";
    }
    return TryResolveFromObjNode(objNode, criteria, scope, encClass);
}

/**
 * @brief Resolves receiver type name from scope symbols when AST lookup is unavailable.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return Cleaned receiver type name, or empty string if not resolvable.
 */
std::string ResolveReceiverFromScope(const std::string& fileUri, const analysis::LocalReference& ref,
                                     const analysis::Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    std::string currentReceiverType;
    for (const auto& candRef : scope->references)
    {
        if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
        {
            if (currentReceiverType.empty())
            {
                std::string enc = (candRef.name == "this") ? GetEnclosingClassName(criteria.symbolTable, fileUri,
                                                                                   ref.startLine, criteria.classCache)
                                                           : "";
                currentReceiverType = ResolveObjectTextType(candRef.name, scope, criteria.symbolTable, enc);
            }
            else
            {
                auto memberSyms = criteria.symbolTable.FindMemberSymbolPtr(currentReceiverType, candRef.name);
                if (memberSyms && !memberSyms->empty())
                {
                    const auto& m = memberSyms->front();
                    if (m.type == analysis::SymbolType::Variable && !m.GetVariable().typeName.empty())
                    {
                        currentReceiverType = analysis::CleanBaseType(m.GetVariable().typeName);
                    }
                    else if (m.type == analysis::SymbolType::Function && !m.GetFunction().returnType.empty())
                    {
                        currentReceiverType = analysis::CleanBaseType(m.GetFunction().returnType);
                    }
                }
                else
                {
                    std::string enc =
                        (candRef.name == "this")
                            ? GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine, criteria.classCache)
                            : "";
                    std::string t = ResolveObjectTextType(candRef.name, scope, criteria.symbolTable, enc);
                    if (!t.empty())
                    {
                        currentReceiverType = t;
                    }
                }
            }
        }
    }
    return currentReceiverType;
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
    std::string rType = ResolveReceiverFromAst(fileUri, ref, scope, criteria);
    if (rType.empty())
    {
        rType = ResolveReceiverFromScope(fileUri, ref, scope, criteria);
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
 * @brief Checks if a qualified member reference matches a target namespace.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference to check.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if reference is qualified by the target namespace.
 */
bool MatchesTargetNamespaceQualifier(const std::string& fileUri, const analysis::LocalReference& ref,
                                     const analysis::Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    if (criteria.targetNamespace.empty())
    {
        return false;
    }
    if (fileUri == criteria.request.uri)
    {
        std::string q = GetMemberObjectText(criteria.request, ref);
        if (q == criteria.targetNamespace || analysis::EndsWithScopedSuffix(q, criteria.targetNamespace) ||
            analysis::EndsWithScopedSuffix(criteria.targetNamespace, q))
        {
            return true;
        }
    }
    if (scope)
    {
        for (const auto& candRef : scope->references)
        {
            if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
            {
                if (candRef.name == criteria.targetNamespace ||
                    analysis::EndsWithScopedSuffix(candRef.name, criteria.targetNamespace) ||
                    analysis::EndsWithScopedSuffix(criteria.targetNamespace, candRef.name))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Checks if a qualifier string matches the target enum container or target namespace.
 * @param[in] q Qualifier string.
 * @param[in] targetEnumContainer Target enum container name.
 * @param[in] targetNamespace Target namespace name.
 * @return True if qualifier matches.
 */
bool QualifierMatchesEnumContainer(std::string_view q, std::string_view targetEnumContainer,
                                   std::string_view targetNamespace)
{
    if (q.empty())
    {
        return false;
    }
    if (q == targetEnumContainer || analysis::EndsWithScopedSuffix(q, targetEnumContainer) ||
        analysis::EndsWithScopedSuffix(targetEnumContainer, q))
    {
        return true;
    }
    return !targetNamespace.empty() && (q == targetNamespace || analysis::EndsWithScopedSuffix(q, targetNamespace));
}

/**
 * @brief Validates member or scoped access to an enum constant.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if member qualifier matches target enum container.
 */
bool IsValidEnumMemberAccess(const std::string& fileUri, const analysis::LocalReference& ref,
                             const analysis::Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    if (ref.isScopedIdentifier && !ref.scopeQualifier.empty())
    {
        return QualifierMatchesEnumContainer(ref.scopeQualifier, criteria.targetEnumContainer,
                                             criteria.targetNamespace);
    }
    if (fileUri == criteria.request.uri)
    {
        std::string q = GetMemberObjectText(criteria.request, ref);
        if (QualifierMatchesEnumContainer(q, criteria.targetEnumContainer, criteria.targetNamespace))
        {
            return true;
        }
    }
    if (scope)
    {
        for (const auto& candRef : scope->references)
        {
            if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
            {
                if (candRef.name == criteria.targetEnumContainer ||
                    analysis::EndsWithScopedSuffix(criteria.targetEnumContainer, candRef.name))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Validates accessibility for enum constant references.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if reference is a valid access to the enum constant.
 */
bool IsValidEnumAccess(const std::string& fileUri, const analysis::LocalReference& ref, const analysis::Scope* scope,
                       const ReferenceCollectionCriteria& criteria)
{
    if (ref.isCall)
    {
        return false;
    }
    if (ref.isMemberAccess)
    {
        return IsValidEnumMemberAccess(fileUri, ref, scope, criteria);
    }

    if (!criteria.targetNamespace.empty())
    {
        if (!IsInsideNamespace(fileUri, ref.startLine, criteria.targetNamespace, criteria.symbolTable,
                               criteria.classCache))
        {
            return false;
        }
    }
    else if (fileUri != criteria.request.uri)
    {
        return false;
    }

    const analysis::LocalDefinition* localShadow = analysis::ResolveInScope(scope, ref.name);
    return !(localShadow && (localShadow->kind == analysis::LocalDefinitionKind::Parameter ||
                             localShadow->kind == analysis::LocalDefinitionKind::Variable));
}

/**
 * @brief Checks whether a reference has valid accessibility and scope visibility.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if access is valid.
 */
bool IsEnclosingClassIncompatible(const std::string& fileUri, uint32_t line,
                                  const ReferenceCollectionCriteria& criteria)
{
    if (criteria.targetAccess == analysis::AccessModifier::Private ||
        criteria.targetAccess == analysis::AccessModifier::Protected)
    {
        std::string encClass = GetEnclosingClassName(criteria.symbolTable, fileUri, line, criteria.classCache);
        if (encClass.empty() ||
            (!criteria.compatibleClasses.contains(encClass) &&
             !criteria.compatibleClasses.contains(std::string(analysis::LastScopeSegment(encClass)))))
        {
            return true;
        }
    }
    return false;
}

bool IsValidConstructorAccess(const std::string& fileUri, const analysis::LocalReference& ref,
                              const analysis::Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    if (IsEnclosingClassIncompatible(fileUri, ref.startLine, criteria))
    {
        return false;
    }
    if (ref.isMemberAccess)
    {
        return MatchesTargetNamespaceQualifier(fileUri, ref, scope, criteria);
    }
    if (!criteria.targetNamespace.empty() &&
        !IsInsideNamespace(fileUri, ref.startLine, criteria.targetNamespace, criteria.symbolTable, criteria.classCache))
    {
        return false;
    }
    if (IsLocalShadowingTarget(scope, ref.name, fileUri, criteria))
    {
        return false;
    }
    return (ref.isCall && MatchesCallArguments(ref, criteria)) || (ref.isTypeSpecifier && criteria.minArgs == 0);
}

bool IsValidAccess(const std::string& fileUri, const analysis::LocalReference& ref, const analysis::Scope* scope,
                   const ReferenceCollectionCriteria& criteria)
{
    if (criteria.isEnumConstant)
    {
        return IsValidEnumAccess(fileUri, ref, scope, criteria);
    }
    const bool isConstructor = criteria.isFunction && !criteria.group.empty() &&
                               criteria.group.front().name == criteria.group.front().containerName;
    if (isConstructor)
    {
        return IsValidConstructorAccess(fileUri, ref, scope, criteria);
    }
    if (!criteria.compatibleClasses.empty())
    {
        if (IsEnclosingClassIncompatible(fileUri, ref.startLine, criteria))
        {
            return false;
        }
        return ref.isMemberAccess ? IsCompatibleMemberAccess(fileUri, ref, scope, criteria)
                                  : IsCompatibleNonMemberAccess(fileUri, ref, scope, criteria);
    }

    if (ref.isMemberAccess)
    {
        return MatchesTargetNamespaceQualifier(fileUri, ref, scope, criteria);
    }

    if (!criteria.targetNamespace.empty())
    {
        if (!IsInsideNamespace(fileUri, ref.startLine, criteria.targetNamespace, criteria.symbolTable,
                               criteria.classCache))
        {
            return false;
        }
    }

    return !IsLocalShadowingTarget(scope, ref.name, fileUri, criteria);
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
    std::string targetNamespace;
    std::string targetEnumContainer;
    bool isEnumConstant = false;
    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenRefs;
};

using TargetIndexMap =
    ankerl::unordered_dense::map<std::string, std::vector<size_t>, analysis::TransparentStringHash, std::equal_to<>>;

/**
 * @brief Context bundle for batched reference counting across scope trees.
 */
struct BatchReferenceContext
{
    BatchReferenceContext(std::span<SymbolLensTarget> t, const TargetIndexMap& map, const CodeLensRequest& req)
        : targets(t), targetsByName(map), request(req)
    {
    }

    std::span<SymbolLensTarget> targets;
    const TargetIndexMap& targetsByName;
    const CodeLensRequest& request;
    mutable EnclosingClassCache classCache;
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
                .targetNamespace = target.targetNamespace,
                .targetEnumContainer = target.targetEnumContainer,
                .isEnumConstant = target.isEnumConstant,
                .targetAccess = target.targetAccess,
                .isFunction = target.isFunction,
                .minArgs = target.minArgs,
                .maxArgs = target.maxArgs,
                .symbolTable = ctx.request.symbolTable,
                .request = ctx.request,
                .classCache = &ctx.classCache,
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
    for (const auto& strRef : scope->stringLiteralCallRefs)
    {
        auto it = ctx.targetsByName.find(strRef.name);
        if (it != ctx.targetsByName.end())
        {
            const uint64_t pos = (static_cast<uint64_t>(strRef.line) << 32) | strRef.column;
            for (size_t targetIdx : it->second)
            {
                ctx.targets[targetIdx].seenRefs.insert({fileUri, pos});
            }
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
void BatchCountReferencesAcrossScopes(std::span<SymbolLensTarget> targets, const TargetIndexMap& targetsByName,
                                      const CodeLensRequest& request)
{
    BatchReferenceContext ctx{targets, targetsByName, request};
    request.scopeIndex.ForEachScopeTree(
        [&](const std::string& fileUri, const std::shared_ptr<const analysis::Scope>& root)
        {
            if (root && !angel_lsp::utils::IsPredefinedFile(fileUri, request.predefinedExtension))
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
    const std::string_view shortName = analysis::LastScopeSegment(mixinName);
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
    cmd.title = i18n::FormatMessage(request.i18n, "codelens.jump-to-physical-source", "Jump to physical source in {}",
                                    filename);
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
/**
 * @brief Evaluates whether a symbol's type is enabled for CodeLens under active configuration.
 */
static bool IsSymbolTypeEnabledForCodeLens(const analysis::Symbol& sym, const config::ServerConfig* config)
{
    if (!config)
    {
        return true;
    }
    if (sym.type == analysis::SymbolType::Function)
    {
        return config->features.codeLensShowOnFunctions;
    }
    if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface ||
        sym.type == analysis::SymbolType::Funcdef)
    {
        return config->features.codeLensShowOnClassesAndInterfaces;
    }
    if (sym.type == analysis::SymbolType::Enum)
    {
        return config->features.codeLensShowOnEnums;
    }
    if (sym.type == analysis::SymbolType::Variable)
    {
        if (std::holds_alternative<analysis::VariableSignature>(sym.signature) && sym.GetVariable().isEnumConstant)
        {
            return config->features.codeLensShowOnEnums;
        }
        return config->features.codeLensShowOnPropertiesAndVariables;
    }
    if (sym.type == analysis::SymbolType::Property)
    {
        return config->features.codeLensShowOnPropertiesAndVariables;
    }
    return true;
}

SymbolGroups CollectSymbolGroups(const CodeLensRequest& request)
{
    SymbolGroups result;
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.fileUri != request.uri || sym.isSynthesized ||
                    !IsSymbolTypeEnabledForCodeLens(sym, request.config))
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
        if (s.type == analysis::SymbolType::Variable &&
            std::holds_alternative<analysis::VariableSignature>(s.signature) && s.GetVariable().isEnumConstant &&
            s.containerKind == analysis::ContainerKind::Enum)
        {
            return s;
        }
    }
    for (const auto& s : symGroup)
    {
        if (s.type == analysis::SymbolType::Variable &&
            std::holds_alternative<analysis::VariableSignature>(s.signature) && s.GetVariable().isEnumConstant &&
            !s.containerName.empty())
        {
            return s;
        }
    }
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
                            if (analysis::CleanBaseTypeView(b) == sym.containerName &&
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
                        if (analysis::CleanBaseTypeView(b) == sym.name)
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
    std::string_view sourceCode{};
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
        if (!s.containerName.empty() && IsContainerClass(s.containerName, ctx.symbolTable))
        {
            compatibleClasses.insert(s.containerName);
            compatibleClasses.insert(std::string(analysis::LastScopeSegment(s.containerName)));
            auto comp = analysis::GetCompatibleMemberClasses(s.containerName, sym.name, targetAccess, ctx.symbolTable);
            for (const auto& c : comp)
            {
                compatibleClasses.insert(c);
            }
        }
    }

    if (!compatibleClasses.empty())
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
        AddSymbolDeclRanges(symbolTable.FindMemberSymbols(c, symName), allDeclRanges);
    }
    for (const auto& s : symGroup)
    {
        if (!s.containerName.empty())
        {
            AddSymbolDeclRanges(symbolTable.FindMemberSymbols(s.containerName, symName), allDeclRanges);
        }
    }
    AddSymbolDeclRanges(symGroup, allDeclRanges);
    return allDeclRanges;
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
    const auto& gs = parser::GetGrammarSymbols();
    TSNode hostNode = ts_node_descendant_for_point_range(hostRoot, hostPt, hostPt);
    for (int depth = 0; depth < constants::k_maxAstClimbDepth && !ts_node_is_null(hostNode);
         ++depth, hostNode = parser::GetParent(hostNode))
    {
        if (ts_node_symbol(hostNode) == gs.symClassDeclaration)
        {
            break;
        }
    }

    TSNode incNode = analysis::FindMixinInclusionNode(hostNode, mixinName, mixinName, request.sourceCode);
    if (!ts_node_is_null(incNode))
    {
        TSPoint sp = ts_node_start_point(incNode);
        TSPoint ep = ts_node_end_point(incNode);
        return lsp::Range{lsp::Position{sp.row, sp.column}, lsp::Position{ep.row, ep.column}};
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
        mixinCmd.title =
            i18n::FormatMessage(request.i18n, "codelens.view-mixin-expansion", "View Mixin Expansion: {}", mixinName);
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
    bool isEnumConstant = false;
    std::string targetEnumContainer;
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
        const auto& var = sym.GetVariable();
        targetAccess = var.modifiers.access;
        isEnumConstant = var.isEnumConstant;
        if (isEnumConstant)
        {
            targetEnumContainer = !var.typeName.empty() ? var.typeName : sym.containerName;
        }
    }

    auto compatibleClasses = CollectCompatibleClasses(symGroup, sym, targetAccess, ctx);
    auto allDeclRanges = CollectAllDeclRanges(sym.name, symGroup, compatibleClasses, ctx.symbolTable);

    std::string targetNamespace;
    if (isEnumConstant)
    {
        targetNamespace = std::string(utils::ParentScopeView(targetEnumContainer));
    }
    else if (!sym.containerName.empty() && !IsContainerClass(sym.containerName, ctx.symbolTable))
    {
        targetNamespace = sym.containerName;
    }

    return SymbolLensTarget{
        .key = key,
        .targetName = sym.name,
        .group = symGroup,
        .compatibleClasses = std::move(compatibleClasses),
        .allDeclRanges = std::move(allDeclRanges),
        .targetNamespace = std::move(targetNamespace),
        .targetEnumContainer = std::move(targetEnumContainer),
        .isEnumConstant = isEnumConstant,
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
 * @param[in] ctx Context bundling mixin map and symbol table.
 * @return Constructed SymbolLensTarget.
 */
SymbolLensTarget CreateClassTarget(const RangeKey& key, const analysis::Symbol& sym,
                                   const std::vector<analysis::Symbol>& symGroup, const CompatibleClassContext& ctx)
{
    DeclRangeSet allDeclRanges;
    AddSymbolDeclRanges(symGroup, allDeclRanges);

    std::string targetNamespace;
    if (!sym.containerName.empty() && !IsContainerClass(sym.containerName, ctx.symbolTable))
    {
        targetNamespace = sym.containerName;
    }

    return SymbolLensTarget{
        .key = key,
        .targetName = sym.name,
        .group = symGroup,
        .compatibleClasses = {},
        .allDeclRanges = std::move(allDeclRanges),
        .targetNamespace = std::move(targetNamespace),
        .targetEnumContainer = {},
        .isEnumConstant = false,
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
            if (IsInterfaceMethod(sym, ctx.symbolTable) || analysis::IsDestructorDeclaration(sym, ctx.sourceCode))
            {
                continue;
            }
            keyToTarget[key] = targets.size();
            targets.push_back(CreateFunctionOrVariableTarget(key, sym, symGroup, ctx));
        }
        else if (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property)
        {
            keyToTarget[key] = targets.size();
            targets.push_back(CreateFunctionOrVariableTarget(key, sym, symGroup, ctx));
        }
        else if (sym.type == analysis::SymbolType::Class)
        {
            keyToTarget[key] = targets.size();
            targets.push_back(CreateClassTarget(key, sym, symGroup, ctx));
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
 * @brief Formats the localized reference count string for a code lens.
 * @param[in] refCount Number of references.
 * @param[in] i18n Optional pointer to I18n translation context.
 * @return Localized reference count string.
 */
std::string FormatReferenceCount(size_t refCount, const i18n::I18n* i18n)
{
    return (refCount == 1) ? i18n::FormatMessage(i18n, "codelens.reference", "1 reference")
                           : i18n::FormatMessage(i18n, "codelens.references", "{} references", refCount);
}

/**
 * @brief Formats the localized implementation count string for a code lens.
 * @param[in] implCount Number of interface implementations.
 * @param[in] i18n Optional pointer to I18n translation context.
 * @return Localized implementation count string.
 */
std::string FormatImplementationCount(size_t implCount, const i18n::I18n* i18n)
{
    return (implCount == 1) ? i18n::FormatMessage(i18n, "codelens.implementation", "1 implementation")
                            : i18n::FormatMessage(i18n, "codelens.implementations", "{} implementations", implCount);
}

/**
 * @brief Appends a reference count lens for a mapped symbol target.
 * @param[in] key Declaration range key.
 * @param[in] sym Primary symbol.
 * @param[in] ctx Assembled CodeLens context.
 * @param[in,out] lenses Output list of code lenses.
 */
void AppendReferenceLens(const RangeKey& key, const analysis::Symbol& sym, const AssembledLensContext& ctx,
                         std::vector<lsp::CodeLens>& lenses)
{
    auto it = ctx.keyToTarget.find(key);
    if (it == ctx.keyToTarget.end())
    {
        return;
    }
    size_t refCount = ctx.targets[it->second].seenRefs.size();
    if (ctx.request.unusedRules &&
        analysis::rules::IsEngineManagedFunction(sym, ctx.request.symbolTable, *ctx.request.unusedRules))
    {
        refCount += 1;
    }
    if (refCount == 0 && ctx.request.config && !ctx.request.config->features.codeLensShowZeroReferences)
    {
        return;
    }
    if (refCount == 0 && sym.name.starts_with("op") && sym.name.size() > 2)
    {
        return;
    }
    lenses.push_back(MakeCommandLens(key, FormatReferenceCount(refCount, ctx.request.i18n)));
}

/**
 * @brief Appends lenses for a function symbol, checking for interface implementations first.
 * @param[in] key Declaration range key.
 * @param[in] sym Function symbol.
 * @param[in] ctx Assembled CodeLens context.
 * @param[in,out] lenses Output list of code lenses.
 */
void AppendFunctionLens(const RangeKey& key, const analysis::Symbol& sym, const AssembledLensContext& ctx,
                        std::vector<lsp::CodeLens>& lenses)
{
    if (analysis::IsDestructorDeclaration(sym, ctx.request.sourceCode))
    {
        return;
    }
    if (IsInterfaceMethod(sym, ctx.request.symbolTable))
    {
        size_t implCount = CountInterfaceMethodImplementations(sym, ctx.request.symbolTable);
        lenses.push_back(MakeCommandLens(key, FormatImplementationCount(implCount, ctx.request.i18n)));
        return;
    }
    AppendReferenceLens(key, sym, ctx, lenses);
}

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
        AppendFunctionLens(key, sym, ctx, lenses);
    }
    else if (sym.type == analysis::SymbolType::Interface)
    {
        size_t implCount = CountInterfaceImplementations(sym, ctx.request.symbolTable);
        lenses.push_back(MakeCommandLens(key, FormatImplementationCount(implCount, ctx.request.i18n)));
    }
    else if (sym.type == analysis::SymbolType::Class)
    {
        AppendReferenceLens(key, sym, ctx, lenses);
        AppendMixinExpansionLenses(sym, ctx.request, lenses);
    }
    else if (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property)
    {
        AppendReferenceLens(key, sym, ctx, lenses);
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
    CompatibleClassContext classCtx{BuildMixinInclusionMap(request.symbolTable), request.symbolTable,
                                    request.sourceCode};

    std::vector<SymbolLensTarget> targets;
    KeyTargetMap keyToTarget;
    CollectReferenceTargets(symGroups, classCtx, targets, keyToTarget);

    if (!targets.empty())
    {
        TargetIndexMap targetsByName;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            targetsByName[targets[i].targetName].push_back(i);
            if (targets[i].targetName.starts_with("get_") || targets[i].targetName.starts_with("set_"))
            {
                targetsByName[targets[i].targetName.substr(4)].push_back(i);
            }
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

    if (request.sourceCode.empty() || !request.tree)
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
