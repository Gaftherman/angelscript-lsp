#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "i18n/i18n.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace angel_lsp::analysis
{
std::string OwnDocComment(const HoverDocLookupContext& ctx, const Symbol& symbol)
{
    if (symbol.fileUri.empty() || symbol.fileUri == ctx.documentUri)
    {
        return ExtractDocComment(ctx.sourceCode, symbol.startLine);
    }

    if (!ctx.sources || !ctx.sources->readDocument)
    {
        return "";
    }

    const std::string* declaringText = ctx.sources->readDocument(symbol.fileUri);
    if (!declaringText)
    {
        return "";
    }

    return ExtractDocComment(*declaringText, symbol.startLine);
}

std::string CompanionDocComment(const HoverDocLookupContext& ctx, const Symbol& symbol)
{
    if (symbol.containerName.empty() || !ctx.symbolTable)
    {
        return "";
    }
    std::string propName = symbol.name;
    if (propName.starts_with("get_") || propName.starts_with("set_"))
    {
        propName = propName.substr(4);
    }
    const std::string candidates[] = {utils::JoinScopedName(symbol.containerName, propName),
                                      utils::JoinScopedName(symbol.containerName, "get_" + propName),
                                      utils::JoinScopedName(symbol.containerName, "set_" + propName)};
    for (const auto& candidateName : candidates)
    {
        if (candidateName == symbol.qualifiedName)
        {
            continue;
        }
        const auto companionSyms = ctx.symbolTable->FindSymbolsPtr(candidateName);
        if (!companionSyms)
        {
            continue;
        }
        for (const auto& comp : *companionSyms)
        {
            std::string doc = OwnDocComment(ctx, comp);
            if (!doc.empty())
            {
                return doc;
            }
        }
    }
    return "";
}

std::string InheritedDocComment(const HoverDocLookupContext& ctx, const Symbol& symbol)
{
    if (symbol.type != SymbolType::Function || symbol.containerName.empty() || !ctx.symbolTable)
    {
        return "";
    }

    for (const auto& ancestor : GetInheritedTypeHierarchy(symbol.containerName, *ctx.symbolTable))
    {
        if (ancestor == symbol.containerName)
        {
            continue;
        }

        const auto inherited = ctx.symbolTable->FindMemberSymbolPtr(ancestor, symbol.name);
        if (!inherited)
        {
            continue;
        }

        for (const auto& candidate : *inherited)
        {
            std::string doc = OwnDocComment(ctx, candidate);
            if (!doc.empty())
            {
                return doc;
            }
        }
    }

    LinkedBaseOptions linkedOpts;
    if (ctx.config)
    {
        linkedOpts.proxyMap = &ctx.config->engineRules.unusedRules.baseClassProxyMap;
        linkedOpts.delegateProps = ctx.config->engineRules.unusedRules.baseClassDelegateProperties;
    }

    const auto linkedSymbols =
        FindOverriddenBaseMethodSymbols(symbol.containerName, symbol.name, *ctx.symbolTable, linkedOpts);
    for (const auto& candidate : linkedSymbols)
    {
        std::string doc = OwnDocComment(ctx, candidate);
        if (!doc.empty())
        {
            return doc;
        }
    }

    return "";
}

/**
 * @brief Collects all member and accessor property names declared for a container key.
 * @param[in] ruleIndex Container rule index.
 * @param[in] key Fully-qualified container type name.
 * @param[out] outNames Destination vector for container member names.
 */
static void CollectContainerMemberNames(const rules::RuleIndex& ruleIndex, std::string_view key,
                                        std::vector<std::string>& outNames)
{
    const auto& members = ruleIndex.Members(key);
    for (const auto& m : members.allMemberNames)
    {
        outNames.push_back(m);
    }
    for (const auto& acc : members.accessorPropertyNames)
    {
        if (std::find(outNames.begin(), outNames.end(), acc) == outNames.end())
        {
            outNames.push_back(acc);
        }
    }
}

std::string DocCommentForSymbol(const HoverDocLookupContext& ctx, const Symbol& symbol)
{
    std::string own = OwnDocComment(ctx, symbol);
    if (!own.empty())
    {
        return own;
    }

    std::string companion = CompanionDocComment(ctx, symbol);
    if (!companion.empty())
    {
        return companion;
    }

    return InheritedDocComment(ctx, symbol);
}

void AppendTypedefDoc(const Symbol& sym, const SymbolTable& symbolTable, const angel_lsp::i18n::I18n* i18n,
                      std::vector<std::string>& docs)
{
    if (sym.type != SymbolType::Typedef)
    {
        return;
    }
    std::string currentBase = sym.GetTypedef().baseType;
    std::string primitiveBase;
    std::unordered_set<std::string> visited;
    visited.insert(sym.name);

    while (!currentBase.empty() && !visited.contains(currentBase))
    {
        visited.insert(currentBase);
        if (parser::primitives::IsPrimitive(currentBase))
        {
            primitiveBase = currentBase;
            break;
        }
        if (auto syms = symbolTable.FindSymbolsPtr(currentBase))
        {
            if (!syms->empty() && (*syms)[0].type == SymbolType::Typedef)
            {
                currentBase = (*syms)[0].GetTypedef().baseType;
                continue;
            }
        }
        break;
    }

    if (!primitiveBase.empty())
    {
        if (auto info = parser::primitives::GetPrimitiveDocInfo(primitiveBase))
        {
            std::string key = "hover-primitive-desc-";
            key += primitiveBase;
            std::string desc = angel_lsp::i18n::FormatMessage(i18n, key, std::string(info->description));
            std::string underlying = angel_lsp::i18n::FormatMessage(i18n, "hover-typedef-underlying",
                                                                    " (underlying type: `{}`)", primitiveBase);
            std::string doc = desc + underlying;
            if (!info->range.empty())
            {
                if (primitiveBase == "bool")
                {
                    doc += "\n\n" +
                           angel_lsp::i18n::FormatMessage(i18n, "hover-primitive-values", "Values: `{}`", info->range);
                }
                else
                {
                    doc += "\n\n" +
                           angel_lsp::i18n::FormatMessage(i18n, "hover-primitive-range", "Range: `{}`", info->range);
                }
            }
            docs.push_back(std::move(doc));
        }
    }
}

void CollectOverloadDocs(const HoverDocLookupContext& ctx, const std::vector<Symbol>& symbols,
                         const std::optional<Symbol>& best, std::vector<std::string>& docs)
{
    if (best)
    {
        std::string d = DocCommentForSymbol(ctx, *best);
        if (!d.empty())
        {
            docs.push_back(std::move(d));
        }
        if (ctx.symbolTable)
        {
            AppendTypedefDoc(*best, *ctx.symbolTable, ctx.i18n, docs);
        }
        return;
    }

    for (const auto& sym : symbols)
    {
        if (ctx.symbolTable)
        {
            AppendTypedefDoc(sym, *ctx.symbolTable, ctx.i18n, docs);
        }
        std::string d = DocCommentForSymbol(ctx, sym);
        if (d.empty())
        {
            continue;
        }
        std::string trimmedD = CollapseWhitespace(d);
        bool alreadyPresent = std::any_of(docs.begin(), docs.end(), [&](const std::string& existing)
                                          { return CollapseWhitespace(existing) == trimmedD; });
        if (!alreadyPresent)
        {
            docs.push_back(std::move(d));
        }
    }
}

void TryAppendFuncdefSignature(std::string_view typeName, const SymbolTable& table, std::string& md)
{
    const std::string_view cleanType = CleanBaseTypeView(typeName);
    if (cleanType.empty() || parser::primitives::IsPrimitive(cleanType))
    {
        return;
    }
    if (auto funcdefSym = FindFuncdefSymbol(cleanType, table))
    {
        std::string funcdefDecl = FormatFunctionDeclaration(*funcdefSym, true);
        if (!funcdefDecl.empty())
        {
            md += "\n";
            md += funcdefDecl;
        }
    }
}

void AppendMixinMembersSection(const Symbol& sym, const SymbolTable& symbolTable, const angel_lsp::i18n::I18n* i18n,
                               std::ostringstream& oss)
{
    if (sym.type != SymbolType::Class || !std::holds_alternative<ClassSignature>(sym.signature))
    {
        return;
    }
    if (!sym.GetClass().modifiers.isMixin)
    {
        return;
    }
    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    const std::string containerKey = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    std::vector<std::string> memberNames;
    CollectContainerMemberNames(*ruleIndex, containerKey, memberNames);
    if (memberNames.empty() && containerKey != sym.name)
    {
        CollectContainerMemberNames(*ruleIndex, sym.name, memberNames);
    }
    if (memberNames.empty())
    {
        return;
    }
    std::sort(memberNames.begin(), memberNames.end());
    oss << "\n\n---\n**" << i18n::FormatMessage(i18n, "hover-mixin-members", "Mixin Members:") << "**\n";
    for (const auto& name : memberNames)
    {
        oss << "\n- `" << name << "`";
    }
}
} // namespace angel_lsp::analysis
