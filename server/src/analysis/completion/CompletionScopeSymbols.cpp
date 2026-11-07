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

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::analysis::completion
{

void AddEnumConstantCandidate(CompletionCollector& collector, const analysis::Symbol& sym)
{
    const auto& var = sym.GetVariable();
    CompletionEntryKind kind = CompletionEntryKind::EnumMember;
    std::string detail = var.typeName.empty() ? sym.name : utils::JoinScopedName(var.typeName, sym.name);
    const bool qualifyEnum =
        !collector.request.config || collector.request.config->features.completionQualifyEnumValues;
    const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
    SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
    std::string sortText = FormatProximitySortText(prox, sym.name);
    CompletionCandidate cand{sym.name, kind, detail, "", sym.qualifiedName, "", std::move(sortText)};
    if (qualifyEnum && !var.typeName.empty())
    {
        cand.insertText = utils::JoinScopedName(var.typeName, sym.name);
    }
    AddItemIfNew(collector, std::move(cand));
}

/**
 * @brief Processes a local scope constant definition (enum constant) and adds completion candidate.
 * @param[in] def Local definition entry.
 * @param[in] scopeNs Enclosing namespace path of the scope.
 * @param[in,out] collector Completion collector context.
 * @param[in] prox Symbol proximity tier.
 */
static void ProcessScopeConstantDefinition(const analysis::LocalDefinition& def, const std::string& scopeNs,
                                           CompletionCollector& collector, SymbolProximity prox)
{
    const std::string symbolKey = utils::JoinScopedName(scopeNs, def.name);
    auto syms = collector.request.symbolTable.FindSymbols(symbolKey);
    if (syms.empty() && !scopeNs.empty())
    {
        syms = collector.request.symbolTable.FindSymbols(def.name);
    }
    bool added = false;
    for (const auto& sym : syms)
    {
        if (sym.type == analysis::SymbolType::Variable && sym.GetVariable().isEnumConstant)
        {
            AddEnumConstantCandidate(collector, sym);
            added = true;
        }
    }
    if (!added)
    {
        std::string sortText = FormatProximitySortText(prox, def.name);
        AddItemIfNew(collector,
                     {def.name, CompletionEntryKind::EnumMember, def.typeName, "", symbolKey, "", std::move(sortText)});
    }
}

/**
 * @brief Processes a single local scope definition and adds completion candidate.
 * @param[in] def Local definition entry.
 * @param[in] scopeNs Enclosing namespace path of the scope.
 * @param[in,out] collector Completion collector context.
 * @param[in] prox Symbol proximity tier.
 */
static void ProcessScopeDefinition(const analysis::LocalDefinition& def, const std::string& scopeNs,
                                   CompletionCollector& collector, SymbolProximity prox)
{
    if (def.kind == analysis::LocalDefinitionKind::Constant)
    {
        ProcessScopeConstantDefinition(def, scopeNs, collector, prox);
        return;
    }
    const std::string symbolKey = utils::JoinScopedName(scopeNs, def.name);
    CompletionEntryKind kind = CompletionEntryKind::Variable;
    bool isCallable = false;
    std::string snippet;
    if (def.kind == analysis::LocalDefinitionKind::Function || def.kind == analysis::LocalDefinitionKind::Method)
    {
        kind = CompletionEntryKind::Function;
        isCallable = true;
        const bool completeParens = ShouldCompleteParens(collector);
        if (collector.request.snippetSupport && completeParens)
        {
            snippet = CallSnippetForName(symbolKey, collector.request.symbolTable);
            if (snippet.empty() && !scopeNs.empty())
            {
                snippet = CallSnippetForName(def.name, collector.request.symbolTable);
            }
        }
    }
    else if (def.kind == analysis::LocalDefinitionKind::Type)
    {
        kind = CompletionEntryKind::Class;
        if (collector.request.snippetSupport)
        {
            snippet = TemplateSnippetForName(symbolKey, collector.request.symbolTable);
            if (snippet.empty() && !scopeNs.empty())
            {
                snippet = TemplateSnippetForName(def.name, collector.request.symbolTable);
            }
        }
    }
    std::string sortText = FormatProximitySortText(prox, def.name);
    AddItemIfNew(collector, {def.name, kind, def.typeName, "", isCallable ? symbolKey : std::string{}, snippet,
                             std::move(sortText)});
}

void CollectScopeDefinitions(const analysis::Scope* innermostScope, CompletionCollector& collector)
{
    if (!innermostScope)
    {
        return;
    }
    const uint32_t cursorByte = static_cast<uint32_t>(
        utils::PositionToOffset(collector.request.sourceCode, collector.request.position.line,
                                collector.request.position.character, utils::PositionEncoding::Utf16));

    bool crossedClosure = false;
    for (const analysis::Scope* cur = innermostScope; cur != nullptr; cur = cur->parent)
    {
        if (cur->kind == analysis::ScopeKind::Closure)
        {
            crossedClosure = true;
        }
        const bool isLocal = (cur->kind == analysis::ScopeKind::Block || cur->kind == analysis::ScopeKind::Function ||
                              cur->kind == analysis::ScopeKind::Closure);
        const SymbolProximity prox = isLocal ? SymbolProximity::Local : SymbolProximity::CurrentFile;
        const std::string scopeNs = BuildScopeNamespacePath(cur, collector.request);
        for (const auto& def : cur->definitions)
        {
            if (def.kind == analysis::LocalDefinitionKind::Variable && cursorByte < def.visibleFromByte)
            {
                continue;
            }
            if (crossedClosure && cur->kind != analysis::ScopeKind::Closure &&
                (def.kind == analysis::LocalDefinitionKind::Variable ||
                 def.kind == analysis::LocalDefinitionKind::Parameter))
            {
                continue;
            }
            ProcessScopeDefinition(def, scopeNs, collector, prox);
        }
    }
}

static void FormatEnclosingMemberItem(const analysis::Symbol& sym, CompletionCollector& collector,
                                      CompletionEntryKind& kind, std::string& detail, std::string& snippet)
{
    if (sym.type == analysis::SymbolType::Function)
    {
        kind = CompletionEntryKind::Method;
        detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
        const bool completeParens = ShouldCompleteParens(collector);
        if (collector.request.snippetSupport && completeParens)
        {
            snippet = sym.GetFunction().parameters.empty() ? (sym.name + "()$0") : (sym.name + "($0)");
        }
    }
    else if (sym.type == analysis::SymbolType::Property || sym.type == analysis::SymbolType::Variable)
    {
        kind =
            (sym.type == analysis::SymbolType::Property) ? CompletionEntryKind::Property : CompletionEntryKind::Field;
        if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
        {
            detail = sym.GetVariable().typeName + " " + sym.name;
        }
    }
}

static void AddAccessorPropertyCandidate(const analysis::Symbol& sym, const std::string& enclosingClassName,
                                         CompletionCollector& collector, SymbolProximity prox)
{
    const int accessorMode = collector.request.config ? collector.request.config->engine.propertyAccessorMode : 2;
    if (accessorMode < 2 || sym.type != analysis::SymbolType::Function)
    {
        return;
    }
    const bool accessorKeywordRequired = (accessorMode == 3);
    const std::string propertyName = analysis::PropertyNameFromAccessor(sym, accessorKeywordRequired);
    if (!propertyName.empty())
    {
        std::string propertyType = analysis::PropertyTypeFromAccessors(analysis::FindPropertyAccessors(
            enclosingClassName, propertyName, collector.request.symbolTable, accessorKeywordRequired));
        std::string propSortText = FormatProximitySortText(prox, propertyName);
        AddItemIfNew(collector, {propertyName, CompletionEntryKind::Property, propertyType, "", sym.qualifiedName, "",
                                 std::move(propSortText)});
    }
}

static void CollectEnclosingMemberCandidate(const analysis::Symbol& sym, const std::string& enclosingClassName,
                                            CompletionCollector& collector)
{
    if (sym.type == analysis::SymbolType::CallReference)
    {
        return;
    }
    if (sym.containerName != enclosingClassName && sym.containerName != analysis::LastScopeSegment(enclosingClassName))
    {
        return;
    }
    if (sym.type == analysis::SymbolType::Variable && sym.GetVariable().isEnumConstant)
    {
        AddEnumConstantCandidate(collector, sym);
        return;
    }
    CompletionEntryKind kind = CompletionEntryKind::Field;
    std::string snippet;
    std::string detail;
    FormatEnclosingMemberItem(sym, collector, kind, detail, snippet);

    const auto prox = DetermineSymbolProximity(sym, collector.request.uri, GetPredefinedExtension(collector.request));
    std::string sortText = FormatProximitySortText(prox, sym.name);
    AddItemIfNew(collector, {sym.name, kind, detail, "", sym.qualifiedName, snippet, std::move(sortText)});

    AddAccessorPropertyCandidate(sym, enclosingClassName, collector, prox);
}

void CollectEnclosingClassMembers(CompletionCollector& collector)
{
    std::string enclosingClassName = FindEnclosingClassName(collector.request);
    if (enclosingClassName.empty())
    {
        return;
    }
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    auto hierarchy = analysis::GetInheritedTypeHierarchy(enclosingClassName, collector.request.symbolTable);
    analysis::ExpandMixinHostHierarchies(enclosingClassName, collector.request.symbolTable, hierarchy);
    for (const auto& clsName : hierarchy)
    {
        const auto& cm = ruleIndex->ClassMembers(clsName);
        for (const auto& key : cm.memberKeys)
        {
            const auto symList = collector.request.symbolTable.FindSymbolsPtr(key);
            if (!symList)
            {
                continue;
            }
            for (const auto& sym : *symList)
            {
                CollectEnclosingMemberCandidate(sym, clsName, collector);
            }
        }
    }
}

} // namespace angel_lsp::analysis::completion
