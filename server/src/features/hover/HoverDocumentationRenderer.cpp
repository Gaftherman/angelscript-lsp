#include "features/hover/HoverDocumentationRenderer.h"

#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "features/hover/HoverSymbolFormatter.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace angel_lsp::features
{
/**
 * @brief Retrieves doc comment from a companion property or getter/setter.
 * @param[in] request Hover request context.
 * @param[in] symbol Symbol being hovered.
 * @return Companion doc comment string, or empty if none found.
 */
static std::string CompanionDocComment(const HoverRequest& request, const analysis::Symbol& symbol)
{
    if (symbol.containerName.empty())
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
        const auto companionSyms = request.symbolTable.FindSymbolsPtr(candidateName);
        if (!companionSyms)
        {
            continue;
        }
        for (const auto& comp : *companionSyms)
        {
            std::string doc = OwnDocComment(request, comp);
            if (!doc.empty())
            {
                return doc;
            }
        }
    }
    return "";
}

/**
 * @brief Retrieves doc comment from an overridden method in ancestor types.
 * @param[in] request Hover request context.
 * @param[in] symbol Symbol being hovered.
 * @return Inherited doc comment string, or empty if none found.
 */
static std::string InheritedDocComment(const HoverRequest& request, const analysis::Symbol& symbol)
{
    if (symbol.type != analysis::SymbolType::Function || symbol.containerName.empty())
    {
        return "";
    }

    for (const auto& ancestor : analysis::GetInheritedTypeHierarchy(symbol.containerName, request.symbolTable))
    {
        if (ancestor == symbol.containerName)
        {
            continue;
        }

        const auto inherited = request.symbolTable.FindMemberSymbolPtr(ancestor, symbol.name);
        if (!inherited)
        {
            continue;
        }

        for (const auto& candidate : *inherited)
        {
            std::string doc = OwnDocComment(request, candidate);
            if (!doc.empty())
            {
                return doc;
            }
        }
    }

    analysis::LinkedBaseOptions linkedOpts;
    if (request.config)
    {
        linkedOpts.proxyMap = &request.config->engineRules.unusedRules.baseClassProxyMap;
        linkedOpts.delegateProps = request.config->engineRules.unusedRules.baseClassDelegateProperties;
    }

    const auto linkedSymbols =
        analysis::FindOverriddenBaseMethodSymbols(symbol.containerName, symbol.name, request.symbolTable, linkedOpts);
    for (const auto& candidate : linkedSymbols)
    {
        std::string doc = OwnDocComment(request, candidate);
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
static void CollectContainerMemberNames(const analysis::rules::RuleIndex& ruleIndex, std::string_view key,
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

std::string OwnDocComment(const HoverRequest& request, const analysis::Symbol& symbol)
{
    if (symbol.fileUri.empty() || symbol.fileUri == request.uri)
    {
        return analysis::ExtractDocComment(request.sourceCode, symbol.startLine);
    }

    if (!request.readDocument)
    {
        return "";
    }

    const std::string* declaringText = request.readDocument(symbol.fileUri);
    if (!declaringText)
    {
        return "";
    }

    return analysis::ExtractDocComment(*declaringText, symbol.startLine);
}

std::string DocCommentForSymbol(const HoverRequest& request, const analysis::Symbol& symbol)
{
    std::string own = OwnDocComment(request, symbol);
    if (!own.empty())
    {
        return own;
    }

    std::string companion = CompanionDocComment(request, symbol);
    if (!companion.empty())
    {
        return companion;
    }

    return InheritedDocComment(request, symbol);
}

void AppendTypedefDoc(const HoverRequest& request, const analysis::Symbol& sym, std::vector<std::string>& docs)
{
    if (sym.type != analysis::SymbolType::Typedef)
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
        if (auto syms = request.symbolTable.FindSymbolsPtr(currentBase))
        {
            if (!syms->empty() && (*syms)[0].type == analysis::SymbolType::Typedef)
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
            std::string desc = angel_lsp::i18n::FormatMessage(request.i18n, key, std::string(info->description));
            std::string underlying = angel_lsp::i18n::FormatMessage(request.i18n, "hover-typedef-underlying",
                                                                    " (underlying type: `{}`)", primitiveBase);
            std::string doc = desc + underlying;
            if (!info->range.empty())
            {
                if (primitiveBase == "bool")
                {
                    doc += "\n\n" + angel_lsp::i18n::FormatMessage(request.i18n, "hover-primitive-values",
                                                                   "Values: `{}`", info->range);
                }
                else
                {
                    doc += "\n\n" + angel_lsp::i18n::FormatMessage(request.i18n, "hover-primitive-range", "Range: `{}`",
                                                                   info->range);
                }
            }
            docs.push_back(std::move(doc));
        }
    }
}

void CollectOverloadDocs(const HoverRequest& request, const std::vector<analysis::Symbol>& symbols,
                         const std::optional<analysis::Symbol>& best, std::vector<std::string>& docs)
{
    if (best)
    {
        std::string d = DocCommentForSymbol(request, *best);
        if (!d.empty())
        {
            docs.push_back(std::move(d));
        }
        AppendTypedefDoc(request, *best, docs);
        return;
    }

    for (const auto& sym : symbols)
    {
        AppendTypedefDoc(request, sym, docs);
        std::string d = DocCommentForSymbol(request, sym);
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

void AppendLocalDoc(const analysis::LocalDefinition& def, std::string_view typeName, const HoverQueryContext& ctx,
                    std::string& md)
{
    std::string doc = analysis::ExtractDocComment(ctx.request.sourceCode, def.startLine);
    if (doc.empty() && IsDirectInitDeclarator(ctx.node))
    {
        TSNode declarator = ctx.node;
        if (!ts_node_is_null(declarator) &&
            ts_node_symbol(declarator) != parser::GetGrammarSymbols().symVariableDeclarator)
        {
            declarator = parser::GetParent(ctx.node);
        }
        if (auto ctorSym = ResolveConstructorForDeclarator(declarator, typeName, ctx))
        {
            doc = DocCommentForSymbol(ctx.request, *ctorSym);
        }
    }
    if (doc.empty())
    {
        const std::string_view cleanType = analysis::CleanBaseTypeView(typeName);
        if (!cleanType.empty() && !parser::primitives::IsPrimitive(cleanType))
        {
            if (auto funcdefSym = analysis::FindFuncdefSymbol(cleanType, ctx.request.symbolTable))
            {
                doc = DocCommentForSymbol(ctx.request, *funcdefSym);
            }
        }
    }
    if (!doc.empty())
    {
        md += "\n\n---\n\n";
        md += doc;
    }
}

void TryAppendFuncdefSignature(std::string_view typeName, const analysis::SymbolTable& table, std::string& md)
{
    const std::string_view cleanType = analysis::CleanBaseTypeView(typeName);
    if (cleanType.empty() || parser::primitives::IsPrimitive(cleanType))
    {
        return;
    }
    if (auto funcdefSym = analysis::FindFuncdefSymbol(cleanType, table))
    {
        std::string funcdefDecl = analysis::FormatFunctionDeclaration(*funcdefSym, true);
        if (!funcdefDecl.empty())
        {
            md += "\n";
            md += funcdefDecl;
        }
    }
}

void AppendMixinMembersSection(const analysis::Symbol& sym, const HoverQueryContext& ctx, std::ostringstream& oss)
{
    if (sym.type != analysis::SymbolType::Class || !std::holds_alternative<analysis::ClassSignature>(sym.signature))
    {
        return;
    }
    if (!sym.GetClass().modifiers.isMixin)
    {
        return;
    }
    const auto ruleIndex = ctx.request.symbolTable.GetRuleIndex();
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
    oss << "\n\n---\n**" << i18n::FormatMessage(ctx.request.i18n, "hover-mixin-members", "Mixin Members:") << "**\n";
    for (const auto& name : memberNames)
    {
        oss << "\n- `" << name << "`";
    }
}

bool IsCallableSymbol(const analysis::Symbol& sym)
{
    return sym.type == analysis::SymbolType::Function ||
           std::holds_alternative<analysis::FunctionSignature>(sym.signature);
}

size_t CountMoreOverloads(const std::vector<analysis::Symbol>& symbols)
{
    if (symbols.size() <= 1 || !IsCallableSymbol(symbols.front()))
    {
        return 0;
    }
    size_t count = 0;
    const std::string_view primaryName = symbols.front().name;
    for (size_t i = 1; i < symbols.size(); ++i)
    {
        if (symbols[i].name == primaryName && IsCallableSymbol(symbols[i]))
        {
            ++count;
        }
    }
    return count;
}

std::string UrlEncodeCommandArgs(std::string_view input)
{
    static constexpr char k_hexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(input.size() * 2);
    for (unsigned char ch : input)
    {
        const bool isUnreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                                  ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (isUnreserved)
        {
            encoded.push_back(static_cast<char>(ch));
        }
        else
        {
            encoded.push_back('%');
            encoded.push_back(k_hexDigits[(ch >> 4) & 0x0F]);
            encoded.push_back(k_hexDigits[ch & 0x0F]);
        }
    }
    return encoded;
}

std::string BuildPeekOverloadsLink(const std::vector<analysis::Symbol>& symbols, const HoverQueryContext& ctx)
{
    std::ostringstream json;
    json << "[{\"uri\":\"" << ctx.request.uri << "\",\"line\":" << ctx.request.position.line
         << ",\"character\":" << ctx.request.position.character << ",\"locations\":[";

    const std::string_view primaryName = symbols.front().name;
    bool first = true;
    for (const auto& sym : symbols)
    {
        if (sym.name != primaryName || !IsCallableSymbol(sym))
        {
            continue;
        }
        if (!first)
        {
            json << ",";
        }
        first = false;
        const std::string_view symUri =
            sym.fileUri.empty() ? std::string_view(ctx.request.uri) : std::string_view(sym.fileUri);
        const uint32_t sLine = sym.selectionRange.startLine > 0 ? sym.selectionRange.startLine : sym.startLine;
        const uint32_t eLine = sym.selectionRange.endLine > 0 ? sym.selectionRange.endLine : sLine;
        json << "{\"uri\":\"" << symUri << "\",\"startLine\":" << sLine
             << ",\"startChar\":" << sym.selectionRange.startCharacter << ",\"endLine\":" << eLine
             << ",\"endChar\":" << sym.selectionRange.endCharacter << "}";
    }
    json << "]}]";

    const std::string label =
        angel_lsp::i18n::FormatMessage(ctx.request.i18n, "hover-peek-overloads", "Peek overloads");
    return "[" + label + "](command:angelscript.peekOverloads?" + UrlEncodeCommandArgs(json.str()) + ")";
}

std::string FormatMoreOverloads(const std::vector<analysis::Symbol>& symbols, size_t count,
                                const HoverQueryContext& ctx)
{
    const std::string summaryText =
        (count == 1)
            ? angel_lsp::i18n::FormatMessage(ctx.request.i18n, "hover-more-overloads-singular", "1 more overload")
            : angel_lsp::i18n::FormatMessage(ctx.request.i18n, "hover-more-overloads-plural", "{} more overloads",
                                             count);

    std::ostringstream oss;
    oss << "**+" << summaryText << "**\n\n" << BuildPeekOverloadsLink(symbols, ctx);
    return oss.str();
}
} // namespace angel_lsp::features
