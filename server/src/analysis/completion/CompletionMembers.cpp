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

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::analysis::completion
{

bool ShouldCompleteParens(const CompletionCollector& collector)
{
    const bool configEnabled =
        !collector.request.config || collector.request.config->features.completionCompleteFunctionParens;
    return configEnabled && !HasTrailingOpenParen(collector.request.sourceCode, collector.request.position);
}

std::string FindEnclosingClassName(const CompletionQuery& request)
{
    std::string fromTable = request.symbolTable.FindEnclosingClassName(request.uri, request.position.line);
    if (!fromTable.empty())
    {
        return fromTable;
    }
    if (request.nodeIndex)
    {
        const uint32_t cursorByte = ByteOffsetAt(request.sourceCode, request.position.line, request.position.character);
        auto containers = request.nodeIndex->CollectEnclosingContainers(cursorByte, request.sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
            {
                return c.qualifiedName.empty() ? c.name : c.qualifiedName;
            }
        }
    }
    return "";
}

static bool IsMixinIncluded(std::string_view hostClass, std::string_view mixinName, const analysis::SymbolTable& table)
{
    if (hostClass.empty() || mixinName.empty())
    {
        return false;
    }
    auto symsPtr = table.FindSymbolsPtr(std::string(hostClass));
    if (!symsPtr)
    {
        return false;
    }
    for (const auto& sym : *symsPtr)
    {
        if (sym.type == analysis::SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(sym.signature))
        {
            for (const auto& m : sym.GetClass().includedMixins)
            {
                if (m == mixinName || utils::LastScopeSegment(m) == mixinName)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Checks if a member symbol is accessible in completion from the cursor context.
 * @param[in] sym Target member symbol.
 * @param[in] typeName Container type name.
 * @param[in] collector Completion collector context.
 * @return True if accessible, false if restricted by private/protected visibility.
 */
static bool IsMemberAccessibleForCompletion(const analysis::Symbol& sym, std::string_view typeName,
                                            const CompletionCollector& collector)
{
    analysis::AccessModifier access = analysis::AccessModifier::Public;
    if (sym.type == analysis::SymbolType::Function)
    {
        access = sym.GetFunction().modifiers.access;
    }
    else if (sym.type == analysis::SymbolType::Variable)
    {
        access = sym.GetVariable().modifiers.access;
    }
    if (access == analysis::AccessModifier::Public)
    {
        return true;
    }
    const std::string enclosingClass = FindEnclosingClassName(collector.request);
    if (enclosingClass.empty())
    {
        return false;
    }
    const std::string_view memberOwner = !sym.containerName.empty() ? sym.containerName : typeName;
    if (access == analysis::AccessModifier::Private)
    {
        return enclosingClass == memberOwner ||
               IsMixinIncluded(enclosingClass, memberOwner, collector.request.symbolTable) ||
               IsMixinIncluded(memberOwner, enclosingClass, collector.request.symbolTable);
    }
    if (access == analysis::AccessModifier::Protected)
    {
        return enclosingClass == memberOwner ||
               analysis::IsDerivedFrom(enclosingClass, memberOwner, collector.request.symbolTable) ||
               IsMixinIncluded(enclosingClass, memberOwner, collector.request.symbolTable) ||
               IsMixinIncluded(memberOwner, enclosingClass, collector.request.symbolTable);
    }
    return false;
}

/**
 * @brief Converts a member symbol into a completion candidate item.
 * @param[in] sym Target member symbol.
 * @param[in] typeName Container type name.
 * @param[in] tCtx Template context bundling bindings and args.
 * @param[in,out] collector Completion collector context.
 */
static void PopulateMemberSymbolCandidate(const analysis::Symbol& sym, std::string_view typeName,
                                          const MemberTemplateContext& tCtx, CompletionCollector& collector)
{
    if (!IsMemberAccessibleForCompletion(sym, typeName, collector))
    {
        return;
    }

    CompletionEntryKind kind = CompletionEntryKind::Field;
    std::string detail;
    std::string snippet;
    if (sym.type == analysis::SymbolType::Function)
    {
        kind = CompletionEntryKind::Method;
        detail = FormatMethodDetail(sym, tCtx.binding, tCtx.templateArgs);
        const bool completeParens = ShouldCompleteParens(collector);
        if (collector.request.snippetSupport && completeParens)
        {
            snippet = sym.GetFunction().parameters.empty() ? (sym.name + "()$0") : (sym.name + "($0)");
        }
    }
    else if (sym.type == analysis::SymbolType::Variable)
    {
        kind = CompletionEntryKind::Field;
        detail = SubstituteTypeParameters(sym.GetVariable().typeName, tCtx.binding, tCtx.templateArgs);
    }
    else if (sym.type == analysis::SymbolType::Property)
    {
        kind = CompletionEntryKind::Property;
    }

    const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
    SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
    std::string sortText = FormatProximitySortText(prox, sym.name);
    AddItemIfNew(collector, {sym.name, kind, detail, "", sym.qualifiedName, std::move(snippet), std::move(sortText)});

    const int accessorMode = collector.request.config ? collector.request.config->engine.propertyAccessorMode : 2;
    if (accessorMode < 2)
    {
        return;
    }
    const bool accessorKeywordRequired = (accessorMode == 3);
    const std::string propertyName = analysis::PropertyNameFromAccessor(sym, accessorKeywordRequired);
    if (!propertyName.empty())
    {
        std::string propertyType = analysis::PropertyTypeFromAccessors(analysis::FindPropertyAccessors(
            typeName, propertyName, collector.request.symbolTable, accessorKeywordRequired));
        propertyType = SubstituteTypeParameters(propertyType, tCtx.binding, tCtx.templateArgs);
        std::string propSortText = FormatProximitySortText(prox, propertyName);
        AddItemIfNew(collector, {propertyName, CompletionEntryKind::Property, propertyType, "", sym.qualifiedName, "",
                                 std::move(propSortText)});
    }
}

/**
 * @brief Collects all member symbols for a container type.
 * @param[in] typeName Container type name.
 * @param[in] binding Template binding information.
 * @param[in] templateArgs Explicit template arguments.
 * @param[in,out] collector Completion collector context.
 */
static void PopulateMembersForType(std::string_view typeName, const MemberTemplateContext& tCtx,
                                   CompletionCollector& collector)
{
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    const auto& members = ruleIndex->Members(typeName);
    for (const auto& key : members.memberKeys)
    {
        const auto symbols = collector.request.symbolTable.FindSymbolsPtr(key);
        if (!symbols)
        {
            continue;
        }
        for (const auto& sym : *symbols)
        {
            if (sym.type == analysis::SymbolType::CallReference)
            {
                continue;
            }
            if (sym.name.starts_with("op") && !tCtx.memberPrefix.starts_with("op"))
            {
                continue;
            }
            if (sym.containerName == typeName && !IsConstructorOrDestructor(sym, typeName))
            {
                PopulateMemberSymbolCandidate(sym, typeName, tCtx, collector);
            }
        }
    }
}

bool IsIndexedPropertyAccessor(const analysis::SymbolTable& table, std::string_view parentType, std::string_view name,
                               std::string_view enclosingClass)
{
    if (name.empty())
    {
        return false;
    }
    char getterBuf[128];
    const std::string_view getterView = utils::ComposeGetterName(name, getterBuf);
    if (getterView.empty())
    {
        return false;
    }

    const std::string_view searchType = parentType.empty() ? std::string_view(enclosingClass) : parentType;
    if (!searchType.empty())
    {
        auto hierarchy = GetInheritedTypeHierarchy(table, searchType);
        for (const auto& cls : hierarchy)
        {
            if (auto syms = table.FindMemberSymbolPtr(cls, getterView))
            {
                for (const auto& sym : *syms)
                {
                    if (sym.type == analysis::SymbolType::Function && !sym.GetFunction().parameters.empty())
                    {
                        return true;
                    }
                }
            }
        }
    }
    if (auto syms = table.FindSymbolsPtr(getterView))
    {
        for (const auto& sym : *syms)
        {
            if (sym.type == analysis::SymbolType::Function && !sym.GetFunction().parameters.empty())
            {
                return true;
            }
        }
    }
    return false;
}

void PopulateHierarchicalMembers(std::string_view rawTypeName, std::string_view memberPrefix,
                                 CompletionCollector& collector)
{
    const auto& arrayContainer = collector.request.symbolTable.GetDefaultArrayType();
    std::string canonicalType = analysis::CanonicalizeArrayType(rawTypeName, arrayContainer);
    std::string baseContainer = FindCanonicalBaseContainer(canonicalType, collector.request.symbolTable);
    auto targetTemplate = analysis::ParseTemplateType(canonicalType);
    const auto binding = analysis::BindTemplateArguments(canonicalType, collector.request.symbolTable);
    MemberTemplateContext tCtx{binding, targetTemplate.templateArgs, memberPrefix};

    auto hierarchy = GetInheritedTypeHierarchy(collector.request.symbolTable, baseContainer);
    for (const auto& typeName : hierarchy)
    {
        PopulateMembersForType(typeName, tCtx, collector);
    }
}

} // namespace angel_lsp::analysis::completion
