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
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief Collects immediate base classes and interfaces for a given type name.
 * @param[in] symbolTable Global symbol table.
 * @param[in] currentTypeName Type name to inspect.
 * @return Vector of base type names.
 */
static std::vector<std::string> CollectDirectBases(const analysis::SymbolTable& symbolTable,
                                                   const std::string& currentTypeName)
{
    std::vector<std::string> bases;
    auto symsPtr = symbolTable.FindSymbolsPtr(currentTypeName);
    std::vector<analysis::Symbol> fallbackSyms;
    if ((!symsPtr || symsPtr->empty()) && !utils::ContainsScopeSeparator(currentTypeName))
    {
        fallbackSyms = symbolTable.FindTypeSymbolsByShortName(currentTypeName);
    }
    const auto& syms = (symsPtr && !symsPtr->empty()) ? *symsPtr : fallbackSyms;
    for (const auto& sym : syms)
    {
        if (sym.type == analysis::SymbolType::Class)
        {
            const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            if (qName != currentTypeName)
            {
                bases.push_back(qName);
            }
            const auto& classSig = sym.GetClass();
            for (const auto& baseName : classSig.bases)
            {
                bases.push_back(baseName);
            }
        }
        else if (sym.type == analysis::SymbolType::Interface)
        {
            const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            if (qName != currentTypeName)
            {
                bases.push_back(qName);
            }
            const auto& ifaceSig = sym.GetInterface();
            for (const auto& baseName : ifaceSig.inheritedInterfaces)
            {
                bases.push_back(baseName);
            }
        }
    }
    return bases;
}

std::vector<std::string> GetInheritedTypeHierarchy(const analysis::SymbolTable& symbolTable,
                                                   const std::string& initialTypeName)
{
    if (initialTypeName.empty())
    {
        return {};
    }
    std::vector<std::string> hierarchy;
    std::unordered_set<std::string> visited;
    std::vector<std::string> worklist;
    worklist.push_back(initialTypeName);

    while (!worklist.empty())
    {
        std::string current = std::move(worklist.back());
        worklist.pop_back();

        if (current.empty() || !visited.insert(current).second)
        {
            continue;
        }
        hierarchy.push_back(current);

        auto bases = CollectDirectBases(symbolTable, current);
        for (auto it = bases.rbegin(); it != bases.rend(); ++it)
        {
            if (!visited.contains(*it))
            {
                worklist.push_back(std::move(*it));
            }
        }
    }
    analysis::ExpandMixinHostHierarchies(initialTypeName, symbolTable, hierarchy);
    return hierarchy;
}

/**
 * @brief Detects if completion was invoked inside a class or interface base inheritance list.
 */
static std::optional<InheritanceContext> DetectInheritanceContext(const CompletionQuery& request,
                                                                  const std::string& prefix)
{
    const std::string text =
        GetBackwardDeclarationText(request.sourceCode, request.position.line, request.position.character);
    static const std::regex inheritanceRegex(R"(\b(class|interface)\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*:[^{;]*$)");
    std::smatch m;
    if (!std::regex_search(text, m, inheritanceRegex))
    {
        return std::nullopt;
    }

    InheritanceContext ctx;
    ctx.declKind = (m[1].str() == "interface") ? analysis::SymbolType::Interface : analysis::SymbolType::Class;
    ctx.declaringClassName = m[2].str();

    static const std::regex scopeRegex(R"(((?:[a-zA-Z_][a-zA-Z0-9_]*::)+)([a-zA-Z_][a-zA-Z0-9_]*)?$)");
    std::smatch scopeMatch;
    if (std::regex_search(prefix, scopeMatch, scopeRegex))
    {
        ctx.qualifier = std::string(utils::StripTrailingScopeSeparator(scopeMatch[1].str()));
    }
    return ctx;
}

/**
 * @brief Completes container members matching class/interface inheritance rules under a scope qualifier.
 */
static void CompleteInheritanceUnderQualifier(const InheritanceContext& ctx, CompletionCollector& collector)
{
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
    const auto& cm = ruleIndex->Members(ctx.qualifier);
    for (const auto& key : cm.memberKeys)
    {
        const auto symList = collector.request.symbolTable.FindSymbolsPtr(key);
        if (!symList)
        {
            continue;
        }
        for (const auto& sym : *symList)
        {
            if (sym.type == analysis::SymbolType::Namespace)
            {
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                std::string sortText = FormatProximitySortText(prox, sym.name);
                AddItemIfNew(collector, {sym.name, CompletionEntryKind::Module, "", "", sym.qualifiedName, "",
                                         std::move(sortText)});
            }
            else if (sym.type == analysis::SymbolType::Interface && sym.name != ctx.declaringClassName)
            {
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                std::string sortText = FormatProximitySortText(prox, sym.name);
                AddItemIfNew(collector, {sym.name, CompletionEntryKind::Interface, "", "", sym.qualifiedName, "",
                                         std::move(sortText)});
            }
            else if (ctx.declKind == analysis::SymbolType::Class && sym.type == analysis::SymbolType::Class &&
                     sym.name != ctx.declaringClassName)
            {
                if (!sym.GetClass().modifiers.isFinal)
                {
                    SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                    std::string sortText = FormatProximitySortText(prox, sym.name);
                    AddItemIfNew(collector, {sym.name, CompletionEntryKind::Class, "", "", sym.qualifiedName, "",
                                             std::move(sortText)});
                }
            }
        }
    }
}

static void CompleteGlobalInheritanceTypes(const InheritanceContext& ctx, CompletionCollector& collector,
                                           std::string_view predefinedExt)
{
    collector.request.symbolTable.ForEachGlobalTypeSymbol(
        [&](const analysis::Symbol& sym)
        {
            if (sym.type == analysis::SymbolType::Interface && sym.name != ctx.declaringClassName)
            {
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                std::string sortText = FormatProximitySortText(prox, sym.name);
                AddItemIfNew(collector, {sym.name, CompletionEntryKind::Interface, "", "", sym.qualifiedName, "",
                                         std::move(sortText)});
            }
            else if (ctx.declKind == analysis::SymbolType::Class && sym.type == analysis::SymbolType::Class &&
                     sym.name != ctx.declaringClassName && !sym.GetClass().modifiers.isFinal)
            {
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                std::string sortText = FormatProximitySortText(prox, sym.name);
                AddItemIfNew(collector, {sym.name, CompletionEntryKind::Class, "", "", sym.qualifiedName, "",
                                         std::move(sortText)});
            }
        });
}

static void CompleteInheritanceNamespaces(CompletionCollector& collector)
{
    for (const auto& ns : CollectVisibleNamespaces(collector.request, nullptr))
    {
        const std::string rootNs = std::string(utils::FirstScopeSegment(ns));
        std::string sortText = FormatProximitySortText(SymbolProximity::ExternalFile, rootNs);
        AddItemIfNew(collector, {rootNs, CompletionEntryKind::Module, "", "", rootNs, "", std::move(sortText)});
    }

    if (const auto ruleIndex = collector.request.symbolTable.GetRuleIndex())
    {
        for (const auto& [container, _] : ruleIndex->byContainer)
        {
            if (!utils::ContainsScopeSeparator(container) && !container.empty())
            {
                auto syms = collector.request.symbolTable.FindSymbolsPtr(container);
                if (syms && std::any_of(syms->begin(), syms->end(), [](const analysis::Symbol& s)
                                        { return s.type == analysis::SymbolType::Namespace; }))
                {
                    std::string sortText = FormatProximitySortText(SymbolProximity::ExternalFile, container);
                    AddItemIfNew(collector,
                                 {container, CompletionEntryKind::Module, "", "", container, "", std::move(sortText)});
                }
            }
        }
    }
}

bool TryCompleteInheritance(const std::string& prefix, CompletionCollector& collector)
{
    auto ctxOpt = DetectInheritanceContext(collector.request, prefix);
    if (!ctxOpt.has_value())
    {
        return false;
    }

    const auto& ctx = *ctxOpt;
    if (!ctx.qualifier.empty())
    {
        CompleteInheritanceUnderQualifier(ctx, collector);
        return true;
    }

    const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
    CompleteGlobalInheritanceTypes(ctx, collector, predefinedExt);
    CompleteInheritanceNamespaces(collector);
    return true;
}

std::string FindCanonicalBaseContainer(const std::string& canonicalType, const analysis::SymbolTable& symbolTable)
{
    auto targetTemplate = analysis::ParseTemplateType(canonicalType);
    return symbolTable.QualifyShortTypeName(targetTemplate.containerName);
}

std::string SubstituteTypeParameters(std::string text, const analysis::TemplateBinding& binding,
                                     const std::vector<std::string>& templateArgs)
{
    if (binding.usable)
    {
        for (size_t i = 0; i < binding.parameters.size(); ++i)
        {
            text = analysis::SubstituteTypeParam(text, binding.parameters[i], binding.arguments[i]);
        }
        return text;
    }
    if (!templateArgs.empty())
    {
        return analysis::SubstituteTypeParam(text, "T", templateArgs[0]);
    }
    return text;
}

} // namespace angel_lsp::analysis::completion
