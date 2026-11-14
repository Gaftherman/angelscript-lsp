#include "analysis/completion/CompletionInternal.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CompletionFacts.h"
#include "analysis/resolution/CompletionReceiverResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief Collects enum members under a scope qualifier.
 * @param[in] qualifier Qualifier type name.
 * @param[in,out] collector Completion collector context.
 */
static void CollectEnumMembersUnderQualifier(std::string_view qualifier, CompletionCollector& collector)
{
    const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
    auto enumMatches = collector.request.symbolTable.FindSymbolsPtr(qualifier);
    if (enumMatches && !enumMatches->empty())
    {
        for (const auto& sym : *enumMatches)
        {
            if (sym.type == analysis::SymbolType::Enum)
            {
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                for (const auto& mem : sym.GetEnum().members)
                {
                    std::string sortText = FormatProximitySortText(prox, mem.name);
                    AddItemIfNew(collector,
                                 {mem.name, CompletionEntryKind::EnumMember, utils::JoinScopedName(qualifier, mem.name),
                                  "", "", "", std::move(sortText)});
                }
            }
        }
    }
    else if (!utils::ContainsScopeSeparator(qualifier))
    {
        auto shortMatches = collector.request.symbolTable.FindTypeSymbolsByShortName(qualifier);
        for (const auto& sym : shortMatches)
        {
            if (sym.type == analysis::SymbolType::Enum)
            {
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                for (const auto& mem : sym.GetEnum().members)
                {
                    std::string sortText = FormatProximitySortText(prox, mem.name);
                    AddItemIfNew(collector,
                                 {mem.name, CompletionEntryKind::EnumMember, utils::JoinScopedName(qualifier, mem.name),
                                  "", "", "", std::move(sortText)});
                }
            }
        }
    }
}

uint32_t ByteOffsetAt(std::string_view sourceCode, uint32_t line, uint32_t character)
{
    return static_cast<uint32_t>(utils::LineStartOffset(sourceCode, line) + character);
}

std::string BuildScopeNamespacePath(const Scope* scope, const CompletionQuery& request)
{
    if (!scope || !request.nodeIndex)
    {
        return "";
    }
    const uint32_t scopeByte = ByteOffsetAt(request.sourceCode, scope->startLine, scope->startCharacter);
    for (const auto& c : request.nodeIndex->CollectEnclosingContainers(scopeByte, request.sourceCode))
    {
        if (c.kind == analysis::ContainerKind::Namespace)
        {
            return c.qualifiedName;
        }
    }
    return "";
}

/**
 * @brief Appends a namespace name to the list if non-empty and not yet present.
 * @param[in,out] out Target namespace list.
 * @param[in] ns Candidate namespace string.
 */
static void AppendUniqueNamespace(std::vector<std::string>& out, std::string_view ns)
{
    if (!ns.empty() && std::find(out.begin(), out.end(), ns) == out.end())
    {
        out.emplace_back(ns);
    }
}

std::vector<std::string> CollectVisibleNamespaces(const CompletionQuery& request, const analysis::Scope* innermostScope)
{
    std::vector<std::string> visible;
    std::vector<std::string> enclosing;
    if (request.nodeIndex)
    {
        const uint32_t cursorByte = ByteOffsetAt(request.sourceCode, request.position.line, request.position.character);
        for (const auto& c : request.nodeIndex->CollectEnclosingContainers(cursorByte, request.sourceCode))
        {
            if (c.kind == analysis::ContainerKind::Namespace)
            {
                AppendUniqueNamespace(enclosing, c.qualifiedName);
                AppendUniqueNamespace(visible, c.qualifiedName);
            }
        }
    }
    for (const analysis::Scope* cur = innermostScope; cur != nullptr; cur = cur->parent)
    {
        if (cur->kind == analysis::ScopeKind::Namespace)
        {
            std::string nsPath = BuildScopeNamespacePath(cur, request);
            AppendUniqueNamespace(enclosing, nsPath);
            AppendUniqueNamespace(visible, nsPath);
        }
    }
    if (request.nodeIndex)
    {
        for (const auto& imported : analysis::CollectUsingNamespaces(*request.nodeIndex, request.sourceCode))
        {
            AppendUniqueNamespace(visible, imported);
            for (const auto& enc : enclosing)
            {
                AppendUniqueNamespace(visible, utils::JoinScopedName(enc, imported));
            }
        }
    }
    return visible;
}

/**
 * @brief Resolves completion item kind and detail for a container member symbol.
 * @param[in] sym Container member symbol.
 * @param[out] kind Computed LSP completion item kind.
 * @param[out] detail Computed detail string.
 */
static void ResolveContainerMemberItemDetails(const analysis::Symbol& sym, CompletionEntryKind& kind,
                                              std::string& detail)
{
    kind = CompletionEntryKind::Variable;
    switch (sym.type)
    {
    case analysis::SymbolType::Function:
        kind = CompletionEntryKind::Function;
        detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
        break;
    case analysis::SymbolType::Variable:
        if (sym.GetVariable().isEnumConstant)
        {
            kind = CompletionEntryKind::EnumMember;
            const auto& var = sym.GetVariable();
            detail = var.typeName.empty() ? sym.qualifiedName : utils::JoinScopedName(var.typeName, sym.name);
        }
        else
        {
            kind = CompletionEntryKind::Variable;
            detail = sym.GetVariable().typeName;
        }
        break;
    case analysis::SymbolType::Class:
        kind = CompletionEntryKind::Class;
        break;
    case analysis::SymbolType::Interface:
        kind = CompletionEntryKind::Interface;
        break;
    case analysis::SymbolType::Enum:
        kind = CompletionEntryKind::Enum;
        break;
    case analysis::SymbolType::Namespace:
        kind = CompletionEntryKind::Module;
        break;
    default:
        break;
    }
}

/**
 * @brief Adds member items of a specific container to the collector.
 * @param[in] container Container name to query.
 * @param[in] qualifier Scope qualifier text.
 * @param[in] ruleIndex Pre-indexed rule information.
 * @param[in,out] collector Completion collector context.
 */
static void AddContainerMembers(std::string_view container, std::string_view qualifier,
                                const analysis::rules::RuleIndex* ruleIndex, CompletionCollector& collector)
{
    const auto& cm = ruleIndex->Members(container);
    for (const auto& key : cm.memberKeys)
    {
        const auto symList = collector.request.symbolTable.FindSymbolsPtr(key);
        if (!symList)
        {
            continue;
        }
        for (const auto& sym : *symList)
        {
            if (sym.type == analysis::SymbolType::CallReference || !IsSymbolVisibleToModule(sym, collector))
            {
                continue;
            }
            if (sym.containerName == container || sym.containerName == qualifier)
            {
                CompletionEntryKind kind = CompletionEntryKind::Variable;
                std::string detail;
                ResolveContainerMemberItemDetails(sym, kind, detail);
                const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                std::string sortText = FormatProximitySortText(prox, sym.name);
                AddItemIfNew(collector,
                             {sym.name, kind, std::move(detail), "", sym.qualifiedName, "", std::move(sortText)});
            }
        }
    }
}

static void CollectContainerMembersUnderQualifier(std::string_view qualifier, CompletionCollector& collector)
{
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }

    AddContainerMembers(qualifier, qualifier, ruleIndex.get(), collector);
    if (!utils::ContainsScopeSeparator(qualifier))
    {
        auto qit = ruleIndex->qualifiedTypesByShortName.find(qualifier);
        if (qit != ruleIndex->qualifiedTypesByShortName.end())
        {
            for (const auto& q : qit->second)
            {
                AddContainerMembers(q, qualifier, ruleIndex.get(), collector);
            }
        }
    }
    for (const auto& ns : CollectVisibleNamespaces(collector.request, nullptr))
    {
        const std::string qualifiedContainer = utils::JoinScopedName(ns, qualifier);
        AddContainerMembers(qualifiedContainer, qualifier, ruleIndex.get(), collector);
    }
}

bool TryCompleteScopeResolution(std::string_view prefix, CompletionCollector& collector)
{
    static const std::regex scopeResolutionRegex(R"(((?:[a-zA-Z_][a-zA-Z0-9_]*::)+)([a-zA-Z_][a-zA-Z0-9_]*)?$)");
    std::match_results<std::string_view::const_iterator> scopeMatch;
    if (!std::regex_search(prefix.begin(), prefix.end(), scopeMatch, scopeResolutionRegex))
    {
        return false;
    }
    std::string qualifier = std::string(utils::StripTrailingScopeSeparator(scopeMatch[1].str()));
    CollectEnumMembersUnderQualifier(qualifier, collector);
    for (const auto& ns : CollectVisibleNamespaces(collector.request, nullptr))
    {
        CollectEnumMembersUnderQualifier(utils::JoinScopedName(ns, qualifier), collector);
    }
    CollectContainerMembersUnderQualifier(qualifier, collector);
    return true;
}

bool TryCompleteTemplateArguments(std::string_view prefix, CompletionCollector& collector)
{
    if (!IsInsideTemplateArguments(prefix))
    {
        return false;
    }
    collector.request.symbolTable.ForEachGlobalTypeSymbol(
        [&](const analysis::Symbol& sym)
        {
            CompletionEntryKind kind = CompletionEntryKind::Class;
            switch (sym.type)
            {
            case analysis::SymbolType::Interface:
                kind = CompletionEntryKind::Interface;
                break;
            case analysis::SymbolType::Enum:
                kind = CompletionEntryKind::Enum;
                break;
            case analysis::SymbolType::Typedef:
            case analysis::SymbolType::Funcdef:
                kind = CompletionEntryKind::TypeParameter;
                break;
            default:
                break;
            }
            const std::string snippet = collector.request.snippetSupport ? TemplateInsertSnippet(sym) : std::string{};
            const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
            SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
            std::string sortText = FormatProximitySortText(prox, sym.name);
            AddItemIfNew(collector, {sym.name, kind, "", "", sym.qualifiedName, snippet, std::move(sortText)});
        });

    for (const auto& primitive : GetPrimitiveTypeNames())
    {
        std::string sortText = FormatProximitySortText(SymbolProximity::Predefined, primitive);
        AddItemIfNew(collector, {primitive, CompletionEntryKind::Keyword, "", "", "", "", std::move(sortText)});
    }
    return true;
}

} // namespace angel_lsp::analysis::completion
