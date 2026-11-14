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
#include "utils/AccessorNames.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::completion
{

static void CollectGlobalFunctionSymbol(const analysis::Symbol& sym, bool accessorsAreProperties,
                                        bool accessorKeywordRequired, CompletionCollector& collector)
{
    std::string detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
    std::string snippet;
    const bool completeParens = ShouldCompleteParens(collector);
    if (collector.request.snippetSupport && completeParens)
    {
        snippet = CallSnippet(sym.name, sym.GetFunction().parameters);
    }
    const auto prox = DetermineSymbolProximity(sym, collector.request.uri, GetPredefinedExtension(collector.request));
    std::string sortText = FormatProximitySortText(prox, sym.name);
    AddItemIfNew(collector, {sym.name, CompletionEntryKind::Function, std::move(detail), "", sym.qualifiedName,
                             std::move(snippet), std::move(sortText)});
    if (accessorsAreProperties)
    {
        const std::string propName = analysis::PropertyNameFromAccessor(sym, accessorKeywordRequired);
        if (!propName.empty())
        {
            std::string propType = analysis::PropertyTypeFromAccessors(analysis::FindGlobalPropertyAccessors(
                propName, collector.request.symbolTable, accessorKeywordRequired));
            std::string propSortText = FormatProximitySortText(prox, propName);
            AddItemIfNew(collector, {propName, CompletionEntryKind::Property, std::move(propType), "",
                                     sym.qualifiedName, "", std::move(propSortText)});
        }
    }
}

bool IsSymbolVisibleToModule(const analysis::Symbol& sym, const CompletionCollector& collector)
{
    const TransparentStringSet* moduleFiles = collector.request.moduleFileUris;
    return moduleFiles == nullptr || moduleFiles->empty() || moduleFiles->contains(sym.fileUri) ||
           utils::IsPredefinedFile(sym.fileUri, GetPredefinedExtension(collector.request));
}

/**
 * @brief Converts a global symbol table symbol into a completion item.
 * @param[in] sym Target global symbol.
 * @param[in] accessorsAreProperties Whether property accessors are exposed.
 * @param[in] accessorKeywordRequired Whether property keyword is required.
 * @param[in,out] collector Completion collector context.
 */
static void CollectGlobalSymbolItem(const analysis::Symbol& sym, bool accessorsAreProperties,
                                    bool accessorKeywordRequired, CompletionCollector& collector)
{
    if (!IsSymbolVisibleToModule(sym, collector))
    {
        return;
    }
    CompletionEntryKind kind = CompletionEntryKind::Variable;
    std::string detail;
    std::string snippet;
    switch (sym.type)
    {
    case analysis::SymbolType::Function:
        CollectGlobalFunctionSymbol(sym, accessorsAreProperties, accessorKeywordRequired, collector);
        return;
    case analysis::SymbolType::Class:
        kind = CompletionEntryKind::Class;
        if (collector.request.snippetSupport)
        {
            snippet = TemplateInsertSnippet(sym);
        }
        if (!snippet.empty())
        {
            detail = sym.name + "<" + std::get<analysis::ClassSignature>(sym.signature).templateParams.front() + ">";
        }
        break;
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
    case analysis::SymbolType::Namespace:
        kind = CompletionEntryKind::Module;
        break;
    case analysis::SymbolType::Variable:
        if (sym.GetVariable().isEnumConstant)
        {
            AddEnumConstantCandidate(collector, sym);
            return;
        }
        kind = CompletionEntryKind::Variable;
        detail = sym.GetVariable().typeName;
        break;
    case analysis::SymbolType::Property:
        kind = CompletionEntryKind::Property;
        break;
    default:
        break;
    }
    const auto prox = DetermineSymbolProximity(sym, collector.request.uri, GetPredefinedExtension(collector.request));
    std::string sortText = FormatProximitySortText(prox, sym.name);
    AddItemIfNew(collector, {sym.name, kind, detail, "", sym.qualifiedName, snippet, std::move(sortText)});
}

/**
 * @brief Checks if a symbol table bucket can contain symbols matching queryPrefix.
 * @param[in] qualifiedName Symbol table bucket key.
 * @param[in] queryPrefix User query prefix.
 * @param[in] accessorsAreProperties Whether get_/set_ accessors can synthesize properties.
 * @return True if bucket should be inspected.
 */
static bool BucketMatchesPrefix(std::string_view qualifiedName, std::string_view queryPrefix,
                                bool accessorsAreProperties)
{
    if (queryPrefix.empty() || utils::CaseInsensitiveStartsWith(qualifiedName, queryPrefix))
    {
        return true;
    }
    if (accessorsAreProperties)
    {
        if (utils::CaseInsensitiveStartsWith(qualifiedName, utils::k_getterPrefix) &&
            utils::CaseInsensitiveStartsWith(std::string_view(qualifiedName).substr(utils::k_getterPrefix.size()),
                                             queryPrefix))
        {
            return true;
        }
        if (utils::CaseInsensitiveStartsWith(qualifiedName, utils::k_setterPrefix) &&
            utils::CaseInsensitiveStartsWith(std::string_view(qualifiedName).substr(utils::k_setterPrefix.size()),
                                             queryPrefix))
        {
            return true;
        }
    }
    return false;
}

void CollectGlobalSymbols(bool accessorsAreProperties, bool accessorKeywordRequired, std::string_view queryPrefix,
                          CompletionCollector& collector)
{
    collector.request.symbolTable.ForEachSymbolWithPrefix(
        queryPrefix,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            if (!BucketMatchesPrefix(qualifiedName, queryPrefix, accessorsAreProperties))
            {
                return;
            }
            for (const auto& sym : symList)
            {
                if (sym.containerName.empty() && sym.type != analysis::SymbolType::CallReference)
                {
                    CollectGlobalSymbolItem(sym, accessorsAreProperties, accessorKeywordRequired, collector);
                }
            }
        });

    if (collector.request.findModuleSymbols)
    {
        for (const auto& [name, detail] : collector.request.findModuleSymbols(queryPrefix))
        {
            AddItemIfNew(collector, {name, CompletionEntryKind::Module, detail, "", name});
        }
    }
}

void CollectVisibleNamespaceSymbols(const analysis::Scope* innermostScope, std::string_view queryPrefix,
                                    CompletionCollector& collector)
{
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    const int accessorMode = collector.request.config ? collector.request.config->engine.propertyAccessorMode : 2;
    const bool accessorsAreProperties = accessorMode >= 2;
    const bool accessorKeywordRequired = accessorMode == 3;
    for (const auto& ns : CollectVisibleNamespaces(collector.request, innermostScope))
    {
        const auto& cm = ruleIndex->Members(ns);
        for (const auto& key : cm.memberKeys)
        {
            const auto symList = collector.request.symbolTable.FindSymbolsPtr(key);
            if (!symList)
            {
                continue;
            }
            for (const auto& sym : *symList)
            {
                if (sym.containerName == ns && sym.type != analysis::SymbolType::CallReference &&
                    BucketMatchesPrefix(sym.name, queryPrefix, accessorsAreProperties))
                {
                    CollectGlobalSymbolItem(sym, accessorsAreProperties, accessorKeywordRequired, collector);
                }
            }
        }
    }
}

} // namespace angel_lsp::analysis::completion
