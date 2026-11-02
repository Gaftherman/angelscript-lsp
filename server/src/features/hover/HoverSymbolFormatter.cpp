#include "features/hover/HoverSymbolFormatter.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/ConversionRankingEngine.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "features/hover/HoverDocumentationRenderer.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/Constants.h"
#include "utils/ScopedNameUtils.h"
#include <algorithm>
#include <sstream>
#include <vector>

namespace angel_lsp::features
{
static std::string FormatFunctionSignature(const analysis::Symbol& sym)
{
    return analysis::FormatFunctionDeclaration(sym);
}

static std::string FormatClassSignature(const analysis::Symbol& sym)
{
    return analysis::FormatTypeDeclaration(sym);
}

static bool IsContainerProperty(std::string_view containerName, const analysis::SymbolTable* symbolTable)
{
    if (containerName.empty() || !symbolTable)
    {
        return !containerName.empty();
    }
    auto containerSyms = symbolTable->FindSymbolsPtr(containerName);
    if (!containerSyms)
    {
        return false;
    }
    for (const auto& cs : *containerSyms)
    {
        if (cs.type == analysis::SymbolType::Class || cs.type == analysis::SymbolType::Interface)
        {
            return true;
        }
    }
    return false;
}

static bool IsVariableProperty(const analysis::Symbol& sym, const analysis::SymbolTable* symbolTable)
{
    if (sym.containerKind == analysis::ContainerKind::Enum)
    {
        return false;
    }
    if (std::holds_alternative<analysis::VariableSignature>(sym.signature) && sym.GetVariable().isEnumConstant)
    {
        return false;
    }
    return IsContainerProperty(sym.containerName, symbolTable);
}

static std::string FormatNamespaceDeclaration(const analysis::Symbol& sym)
{
    if (!sym.qualifiedName.empty())
    {
        return "namespace " + sym.qualifiedName;
    }
    if (!sym.containerName.empty())
    {
        return "namespace " + utils::JoinScopedName(sym.containerName, sym.name);
    }
    return "namespace " + sym.name;
}

static std::string FormatVariableOrProperty(const analysis::Symbol& sym, const analysis::SymbolTable* symbolTable)
{
    if (sym.containerKind == analysis::ContainerKind::Enum || sym.GetVariable().isEnumConstant)
    {
        return FormatVariableSignature(sym, "(enum member) ");
    }
    const bool isProperty = IsVariableProperty(sym, symbolTable);
    return FormatVariableSignature(sym, isProperty ? "(property) " : "(global variable) ");
}

static std::string FormatSymbolBody(const analysis::Symbol& sym, const analysis::SymbolTable* symbolTable)
{
    switch (sym.type)
    {
    case analysis::SymbolType::Function:
    case analysis::SymbolType::Funcdef:
        return FormatFunctionSignature(sym);
    case analysis::SymbolType::Class:
    case analysis::SymbolType::Interface:
        return FormatClassSignature(sym);
    case analysis::SymbolType::Enum:
    {
        const std::string enumName =
            !sym.containerName.empty() ? utils::JoinScopedName(sym.containerName, sym.name) : sym.name;
        return "enum " + enumName;
    }
    case analysis::SymbolType::Variable:
        return FormatVariableOrProperty(sym, symbolTable);
    case analysis::SymbolType::Typedef:
    {
        const std::string tdName =
            !sym.containerName.empty() ? utils::JoinScopedName(sym.containerName, sym.name) : sym.name;
        return "typedef " + sym.GetTypedef().baseType + " " + tdName;
    }
    case analysis::SymbolType::Namespace:
        return FormatNamespaceDeclaration(sym);
    case analysis::SymbolType::Property:
        if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
        {
            return FormatVariableSignature(sym, "(property) ");
        }
        return "(property) " + sym.name;
    default:
        return sym.name;
    }
}

static bool MatchesTargetNode(TSNode candidate, TSNode target)
{
    return !ts_node_is_null(candidate) &&
           (ts_node_eq(candidate, target) || ts_node_start_byte(candidate) == ts_node_start_byte(target));
}

static TSNode FindCallFromParent(TSNode parent, TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pType = ts_node_symbol(parent);
    if (pType == gs.symCallExpression)
    {
        TSNode fn = parser::GetChildByField(parent, gs.fieldFunction);
        if (MatchesTargetNode(fn, node))
        {
            return parent;
        }
    }
    else if (pType == gs.symConstructCallExpression)
    {
        TSNode typeChild = parser::GetChildByField(parent, gs.fieldType);
        if (MatchesTargetNode(typeChild, node))
        {
            return parent;
        }
    }
    return TSNode{};
}

static std::optional<analysis::Symbol> FindMatchingGlobalSymbol(const analysis::LocalDefinition& def,
                                                                const HoverQueryContext& ctx)
{
    auto candidates = analysis::FindSymbolsInScope(def.name, ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    for (const auto& cand : candidates)
    {
        if (cand.type == analysis::SymbolType::Variable && cand.fileUri == ctx.request.uri)
        {
            return cand;
        }
    }
    for (const auto& cand : candidates)
    {
        if (cand.type == analysis::SymbolType::Variable)
        {
            return cand;
        }
    }
    auto exactCandidates = ctx.request.symbolTable.FindSymbolsPtr(def.name);
    if (exactCandidates)
    {
        for (const auto& cand : *exactCandidates)
        {
            if (cand.type == analysis::SymbolType::Variable && cand.fileUri == ctx.request.uri)
            {
                return cand;
            }
        }
    }
    return std::nullopt;
}

static const analysis::Scope* FindDefinitionScope(const HoverQueryContext& ctx, const analysis::LocalDefinition& def)
{
    analysis::ScopeTraversalGuard cycleGuard;
    for (const analysis::Scope* s = ctx.scope; s != nullptr; s = s->parent)
    {
        if (!cycleGuard.CheckAndInsert(s))
        {
            break;
        }
        for (const auto& d : s->definitions)
        {
            if (d.name == def.name && d.startLine == def.startLine && d.startCharacter == def.startCharacter)
            {
                return s;
            }
        }
    }
    return analysis::FindScopeDeclaringDefinition(ctx.rootScope.get(), def);
}

static void FormatGlobalVariableHover(const analysis::LocalDefinition& def, std::string_view typeName,
                                      const HoverQueryContext& ctx, std::string& md)
{
    auto globalSym = FindMatchingGlobalSymbol(def, ctx);
    if (globalSym.has_value())
    {
        md += FormatVariableSignature(*globalSym, "(global variable) ");
        return;
    }
    md += "(global variable) ";
    if (!typeName.empty())
    {
        md += typeName;
        md += " ";
    }
    md += def.name;
    if (!def.defaultValue.empty())
    {
        md += " = ";
        md += def.defaultValue;
    }
}

static bool IsScopeQualifier(TSNode node) noexcept
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symScope)
    {
        return true;
    }
    if (parentSym == gs.symScopedIdentifier)
    {
        TSNode next = ts_node_next_sibling(node);
        int siblingHops = 0;
        while (!ts_node_is_null(next) && siblingHops++ < 8)
        {
            if (ts_node_symbol(next) == parser::GetGrammarSymbols().tokColonColon)
            {
                return true;
            }
            next = ts_node_next_sibling(next);
        }
    }
    return false;
}

static bool IsTypeSymbol(const analysis::Symbol& sym) noexcept
{
    return sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface ||
           sym.type == analysis::SymbolType::Enum || sym.type == analysis::SymbolType::Funcdef ||
           sym.type == analysis::SymbolType::Typedef;
}

static void PromoteConstructorsToTypes(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    std::vector<analysis::Symbol> promoted;
    for (const auto& s : symbols)
    {
        if (s.type == analysis::SymbolType::Function && !s.containerName.empty() &&
            analysis::LastScopeSegment(s.containerName) == s.name)
        {
            if (auto classSyms = ctx.request.symbolTable.FindSymbolsPtr(s.containerName))
            {
                for (const auto& cs : *classSyms)
                {
                    if (cs.type == analysis::SymbolType::Class || cs.type == analysis::SymbolType::Interface)
                    {
                        promoted.push_back(cs);
                    }
                }
            }
        }
    }
    symbols.insert(symbols.end(), promoted.begin(), promoted.end());
}

static TSNode FindNextScopedMemberNode(TSNode parent, TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symScopedIdentifier)
    {
        return TSNode{};
    }
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    bool foundCurrent = false;
    TSNode nextMember{};
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode current = ts_tree_cursor_current_node(&cursor);
            if (foundCurrent && ts_node_is_named(current))
            {
                nextMember = current;
                break;
            }
            if (!foundCurrent && ts_node_is_named(current) &&
                (ts_node_eq(current, node) || ts_node_start_byte(current) == ts_node_start_byte(node)))
            {
                foundCurrent = true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return nextMember;
}

static bool MemberBelongsToClass(const analysis::Symbol& classSym, std::string_view memberName,
                                 const analysis::SymbolTable& table)
{
    const std::string qName = classSym.qualifiedName.empty() ? classSym.name : classSym.qualifiedName;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(qName, table);
    for (const auto& cls : hierarchy)
    {
        if (const auto found = table.FindMemberSymbolPtr(cls, memberName))
        {
            for (const auto& sym : *found)
            {
                if (sym.containerKind == analysis::ContainerKind::Class ||
                    sym.containerKind == analysis::ContainerKind::Interface)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool MemberBelongsToNamespace(const analysis::Symbol& nsSym, std::string_view memberName,
                                     const analysis::SymbolTable& table)
{
    const std::string qName = nsSym.qualifiedName.empty() ? nsSym.name : nsSym.qualifiedName;
    if (const auto found = table.FindMemberSymbolPtr(qName, memberName))
    {
        for (const auto& sym : *found)
        {
            if (sym.containerKind == analysis::ContainerKind::Namespace)
            {
                return true;
            }
        }
    }
    return false;
}

static void FilterAndAddConstructorCandidates(const std::vector<analysis::Symbol>* list,
                                              const analysis::Symbol& classSym, const std::string& qName,
                                              std::vector<analysis::Symbol>& ctorSymbols)
{
    if (!list)
    {
        return;
    }
    for (const auto& s : *list)
    {
        if (s.type == analysis::SymbolType::Function && s.name == classSym.name &&
            (s.containerName == qName || s.containerName == classSym.name ||
             analysis::LastScopeSegment(s.containerName) == classSym.name))
        {
            if (std::none_of(ctorSymbols.begin(), ctorSymbols.end(), [&](const analysis::Symbol& existing)
                             { return existing.name == s.name && analysis::HasSameParameterList(existing, s); }))
            {
                ctorSymbols.push_back(s);
            }
        }
    }
}

static void CollectClassConstructors(const analysis::Symbol& sym, const HoverQueryContext& ctx,
                                     std::vector<analysis::Symbol>& ctorSymbols)
{
    const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    auto ctors = ctx.request.symbolTable.FindMemberSymbolPtr(qName, sym.name);
    if ((!ctors || ctors->empty()) && qName != sym.name)
    {
        ctors = ctx.request.symbolTable.FindMemberSymbolPtr(sym.name, sym.name);
    }
    if (ctors)
    {
        ctorSymbols.insert(ctorSymbols.end(), ctors->begin(), ctors->end());
    }

    FilterAndAddConstructorCandidates(ctx.request.symbolTable.FindSymbolsPtr(sym.name).get(), sym, qName, ctorSymbols);
    if (qName != sym.name)
    {
        FilterAndAddConstructorCandidates(ctx.request.symbolTable.FindSymbolsPtr(qName).get(), sym, qName, ctorSymbols);
    }
}

std::string CollapseWhitespace(std::string_view text)
{
    std::string result;
    result.reserve(text.size());

    bool inWhitespace = false;
    for (char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (!inWhitespace)
            {
                result.push_back(' ');
                inWhitespace = true;
            }
        }
        else
        {
            result.push_back(c);
            inWhitespace = false;
        }
    }

    auto start = result.find_first_not_of(' ');
    if (start == std::string::npos)
    {
        return "";
    }
    auto end = result.find_last_not_of(' ');
    return result.substr(start, end - start + 1);
}

std::string ExtractDeclarationTextAt(TSNode root, const std::string& sourceCode, TSPoint point, TSSymbol wantedSymbol)
{
    if (ts_node_is_null(root))
    {
        return "";
    }

    TSNode current = ts_node_descendant_for_point_range(root, point, point);
    TSNode declNode = parser::FindAncestorWithSymbol(current, wantedSymbol);
    if (!ts_node_is_null(declNode))
    {
        const uint32_t start = ts_node_start_byte(declNode);
        const uint32_t end = ts_node_end_byte(declNode);
        if (start < end && end <= sourceCode.size())
        {
            return CollapseWhitespace(sourceCode.substr(start, end - start));
        }
    }
    return "";
}

std::string FormatVariableSignature(const analysis::Symbol& sym, const char* role)
{
    if (sym.type != analysis::SymbolType::Variable && sym.type != analysis::SymbolType::Property)
    {
        return std::string(role) + sym.name;
    }
    if (!std::holds_alternative<analysis::VariableSignature>(sym.signature))
    {
        return std::string(role) + sym.name;
    }
    if (sym.containerKind == analysis::ContainerKind::Enum || sym.GetVariable().isEnumConstant)
    {
        role = "(enum member) ";
        return std::string(role) + analysis::FormatVariableDeclaration(sym.GetVariable(), sym.name);
    }
    std::string displayName = sym.name;
    if (sym.containerKind == analysis::ContainerKind::Namespace && !sym.containerName.empty())
    {
        displayName = utils::JoinScopedName(sym.containerName, sym.name);
    }
    return std::string(role) + analysis::FormatVariableDeclaration(sym.GetVariable(), displayName);
}

std::string FormatDeclarationText(const analysis::Symbol& sym, const analysis::SymbolTable* symbolTable)
{
    std::string metaPrefix;
    for (const auto& meta : sym.metadata)
    {
        metaPrefix += meta + "\n";
    }
    return metaPrefix + FormatSymbolBody(sym, symbolTable);
}

void RemoveDuplicateSymbols(std::vector<analysis::Symbol>& symbols)
{
    std::vector<analysis::Symbol> unique;
    unique.reserve(symbols.size());
    for (const auto& sym : symbols)
    {
        const bool duplicate = std::any_of(unique.begin(), unique.end(),
                                           [&](const analysis::Symbol& kept)
                                           {
                                               if (kept.name != sym.name || kept.type != sym.type)
                                                   return false;
                                               if (!sym.fileUri.empty() && !kept.fileUri.empty() &&
                                                   sym.fileUri == kept.fileUri && sym.startLine == kept.startLine)
                                                   return true;
                                               return FormatDeclarationText(kept) == FormatDeclarationText(sym);
                                           });
        if (!duplicate)
        {
            unique.push_back(sym);
        }
    }
    symbols = std::move(unique);
}

TSNode FindEnclosingCallNode(TSNode node)
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }

    if (TSNode call = FindCallFromParent(parent, node); !ts_node_is_null(call))
    {
        return call;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pType = ts_node_symbol(parent);
    if (pType == gs.symDatatype || pType == gs.symMemberExpression || pType == gs.symScopedIdentifier)
    {
        TSNode grandParent = parser::GetParent(parent);
        if (!ts_node_is_null(grandParent))
        {
            if (TSNode call = FindCallFromParent(grandParent, parent); !ts_node_is_null(call))
            {
                return call;
            }
        }
    }
    return TSNode{};
}

std::optional<analysis::Symbol> ResolveCallOverload(TSNode node, const std::vector<analysis::Symbol>& candidates,
                                                    const HoverRequest& request, const analysis::Scope* scope)
{
    if (candidates.empty())
    {
        return std::nullopt;
    }
    if (candidates.size() == 1)
    {
        return candidates.front();
    }

    TSNode callNode = FindEnclosingCallNode(node);
    if (ts_node_is_null(callNode))
    {
        return std::nullopt;
    }

    std::vector<std::string> argTypes =
        analysis::ExtractCallArgumentTypes(callNode, {scope, request.symbolTable, request.sourceCode, request.uri});

    auto match = analysis::ResolveBestOverload(candidates, argTypes, request.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return *match.bestCandidate;
    }
    if (match.priorityCandidate != nullptr)
    {
        return *match.priorityCandidate;
    }

    const analysis::Symbol* fallback = analysis::FindBestFallbackOverload(candidates, argTypes, request.symbolTable);
    if (fallback != nullptr)
    {
        return *fallback;
    }
    if (!candidates.empty())
    {
        return candidates.front();
    }
    return std::nullopt;
}

std::optional<analysis::Symbol> ResolveOrMatchCandidate(std::vector<analysis::Symbol>& symbols,
                                                        const HoverQueryContext& ctx)
{
    auto best = ResolveCallOverload(ctx.node, symbols, ctx.request, ctx.scope);
    if (!best)
    {
        auto it = std::find_if(symbols.begin(), symbols.end(),
                               [&](const analysis::Symbol& s)
                               {
                                   if (s.fileUri != ctx.request.uri)
                                   {
                                       return false;
                                   }
                                   return s.startLine == ctx.queryLine || s.selectionRange.startLine == ctx.queryLine;
                               });
        if (it != symbols.end())
        {
            best = *it;
            std::rotate(symbols.begin(), it, it + 1);
        }
        else if (!symbols.empty())
        {
            best = symbols.front();
        }
    }
    else
    {
        auto it = std::find_if(symbols.begin(), symbols.end(), [&](const analysis::Symbol& s)
                               { return s.name == best->name && analysis::HasSameParameterList(s, *best); });
        if (it != symbols.end())
        {
            std::rotate(symbols.begin(), it, it + 1);
        }
    }
    return best;
}

std::string FormatSynthesizedPropertyHeader(const std::vector<analysis::Symbol>& symbols,
                                            std::string_view accessorPropertyType, std::string_view nodeText)
{
    if (accessorPropertyType.empty())
    {
        return "";
    }
    std::string indexParam;
    for (const auto& sym : symbols)
    {
        if (sym.type == analysis::SymbolType::Function &&
            std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            const auto& fn = sym.GetFunction();
            if ((sym.name.starts_with("get_") && !fn.parameters.empty()) ||
                (sym.name.starts_with("set_") && fn.parameters.size() > 1))
            {
                indexParam = fn.parameters.front().typeName;
                if (!fn.parameters.front().name.empty())
                {
                    indexParam += " " + fn.parameters.front().name;
                }
                break;
            }
        }
    }
    std::ostringstream oss;
    if (!indexParam.empty())
    {
        oss << "(property) " << accessorPropertyType << " " << nodeText << "[" << indexParam << "]\n";
    }
    else
    {
        oss << "(property) " << accessorPropertyType << " " << nodeText << "\n";
    }
    return oss.str();
}

lsp::Hover FormatSymbolsHover(std::vector<analysis::Symbol>& symbols, std::string_view accessorPropertyType,
                              const HoverQueryContext& ctx)
{
    auto best = ResolveOrMatchCandidate(symbols, ctx);
    RemoveDuplicateSymbols(symbols);
    if (!symbols.empty() && ctx.profiler.IsActive())
    {
        ctx.profiler.symbolName = symbols.front().name;
    }

    utils::HighResTimer fmtTimer;
    std::ostringstream oss;
    oss << "```angelscript\n";
    oss << FormatSynthesizedPropertyHeader(symbols, accessorPropertyType, ctx.nodeText);

    if (!symbols.empty())
    {
        oss << FormatDeclarationText(symbols.front(), &ctx.request.symbolTable);
    }
    oss << "\n```";

    const size_t moreCount = CountMoreOverloads(symbols);
    if (moreCount > 0)
    {
        oss << "\n\n" << FormatMoreOverloads(symbols, moreCount, ctx);
    }

    std::vector<std::string> docs;
    CollectOverloadDocs(ctx.request, symbols, best, docs);
    if (!docs.empty())
    {
        oss << "\n\n---";
        for (const auto& d : docs)
        {
            oss << "\n\n" << d;
        }
    }

    if (!symbols.empty())
    {
        AppendMixinMembersSection(symbols.front(), ctx, oss);
    }

    if (ctx.request.config && ctx.request.config->features.hoverShowFileLocation && !symbols.empty() &&
        !symbols.front().fileUri.empty())
    {
        oss << "\n\n---\n*(" << symbols.front().fileUri << ")*";
    }

    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str()}, ctx.range};
}

std::optional<lsp::Hover> FormatMemberHover(std::vector<analysis::Symbol>& memberSymbols,
                                            std::string_view accessorPropertyType, const HoverQueryContext& ctx)
{
    return FormatSymbolsHover(memberSymbols, accessorPropertyType, ctx);
}

void FormatParameterHover(const analysis::LocalDefinition& def, std::string_view typeName, const HoverQueryContext& ctx,
                          std::string& md)
{
    md += "(parameter) ";
    TSNode rootNode = ts_tree_root_node(ctx.request.tree);
    const std::string declText =
        ExtractDeclarationTextAt(rootNode, ctx.request.sourceCode, {def.startLine, def.startCharacter},
                                 parser::GetGrammarSymbols().symParameter);
    if (!declText.empty())
    {
        md += declText;
    }
    else
    {
        if (!typeName.empty())
        {
            md += typeName;
            md += " ";
        }
        md += def.name;
        if (!def.defaultValue.empty())
        {
            md += " = ";
            md += def.defaultValue;
        }
    }

    TryAppendFuncdefSignature(typeName, ctx.request.symbolTable, md);
}

bool IsDirectInitDeclarator(TSNode node)
{
    TSNode declarator = node;
    if (!ts_node_is_null(declarator) && ts_node_symbol(declarator) != parser::GetGrammarSymbols().symVariableDeclarator)
    {
        declarator = parser::GetParent(node);
    }
    if (ts_node_is_null(declarator) || ts_node_symbol(declarator) != parser::GetGrammarSymbols().symVariableDeclarator)
    {
        return false;
    }
    TSNode args = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldArguments);
    return !ts_node_is_null(args);
}

std::optional<analysis::Symbol> ResolveConstructorForDeclarator(TSNode declarator, std::string_view typeName,
                                                                const HoverQueryContext& ctx)
{
    TSNode argList = analysis::ResolveArgumentListNode(declarator);
    if (ts_node_is_null(argList))
    {
        return std::nullopt;
    }

    std::string_view baseName = analysis::CleanBaseTypeView(typeName);
    if (baseName.empty() || parser::primitives::IsPrimitive(baseName))
    {
        return std::nullopt;
    }

    auto candidates = analysis::CollectConstructorCandidates(std::string(baseName), declarator, ctx.request.sourceCode,
                                                             ctx.request.symbolTable);
    if (candidates.empty())
    {
        return std::nullopt;
    }

    if (candidates.size() == 1)
    {
        return candidates[0];
    }

    auto argTypes = analysis::ExtractCallArgumentTypes(
        argList, {ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri});
    auto match = analysis::ResolveBestOverload(candidates, argTypes, ctx.request.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return *match.bestCandidate;
    }
    if (match.priorityCandidate != nullptr)
    {
        return *match.priorityCandidate;
    }

    const analysis::Symbol* fallback =
        analysis::FindBestFallbackOverload(candidates, argTypes, ctx.request.symbolTable);
    if (fallback != nullptr)
    {
        return *fallback;
    }

    return candidates[0];
}

void TryAppendConstructorSignature(TSNode node, std::string_view typeName, const HoverQueryContext& ctx,
                                   std::string& md)
{
    TSNode declarator = node;
    if (!ts_node_is_null(declarator) && ts_node_symbol(declarator) != parser::GetGrammarSymbols().symVariableDeclarator)
    {
        declarator = parser::GetParent(node);
    }
    if (!ts_node_is_null(declarator) && ts_node_symbol(declarator) == parser::GetGrammarSymbols().symVariableDeclarator)
    {
        if (auto ctorSym = ResolveConstructorForDeclarator(declarator, typeName, ctx))
        {
            std::string ctorSig = analysis::FormatFunctionDeclaration(*ctorSym);
            if (!ctorSig.empty())
            {
                md += "\n";
                md += ctorSig;
            }
        }
    }
}

void FormatVariableHover(const analysis::LocalDefinition& def, std::string_view typeName, const HoverQueryContext& ctx,
                         std::string& md)
{
    const analysis::Scope* declaringScope = FindDefinitionScope(ctx, def);

    bool isInsideFunction = false;
    analysis::ScopeTraversalGuard cycleGuard;
    for (const analysis::Scope* s = declaringScope; s != nullptr; s = s->parent)
    {
        if (!cycleGuard.CheckAndInsert(s))
        {
            break;
        }
        if (s->isFunctionScope)
        {
            isInsideFunction = true;
            break;
        }
    }

    if (!isInsideFunction)
    {
        FormatGlobalVariableHover(def, typeName, ctx, md);
    }
    else
    {
        md += "(local variable) ";
        if (!typeName.empty())
        {
            md += typeName;
            md += " ";
        }
        md += def.name;
        if (analysis::IsSimpleLiteralExpression(def.defaultValue))
        {
            md += " = ";
            md += def.defaultValue;
        }
    }

    const size_t prevLen = md.size();
    TryAppendFuncdefSignature(typeName, ctx.request.symbolTable, md);
    if (md.size() == prevLen && IsDirectInitDeclarator(ctx.node))
    {
        TryAppendConstructorSignature(ctx.node, typeName, ctx, md);
    }
}

std::optional<lsp::Hover> FormatNamedArgHover(const std::vector<analysis::Symbol>& candidates,
                                              const HoverQueryContext& ctx)
{
    for (const auto& sym : candidates)
    {
        if (sym.type != analysis::SymbolType::Function ||
            !std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            continue;
        }
        for (const auto& param : sym.GetFunction().parameters)
        {
            if (param.name != ctx.nodeText)
            {
                continue;
            }
            std::string md = "```angelscript\n(parameter) " + param.typeName + " " + param.name;
            if (!param.defaultValue.empty())
            {
                md += " = " + param.defaultValue;
            }
            md += "\n```\n" + angel_lsp::i18n::FormatMessage(ctx.request.i18n, "hover-named-arg-parameter-of",
                                                             "Parameter of `{}`", sym.name);
            return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)},
                              ctx.range};
        }
    }
    return std::nullopt;
}

ContainerContextPreference CheckContextPreference(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (IsScopeQualifier(node))
    {
        TSNode usingDecl = parser::FindAncestorWithSymbol(parser::GetParent(node), gs.symUsingDeclaration);
        return !ts_node_is_null(usingDecl) ? ContainerContextPreference::PreferNamespace
                                           : ContainerContextPreference::None;
    }

    ContainerContextPreference pref = ContainerContextPreference::None;
    parser::ForEachAncestor(parser::GetParent(node),
                            [&](TSNode cur) -> bool
                            {
                                const TSSymbol sym = ts_node_symbol(cur);
                                if (sym == gs.symDatatype || sym == gs.symTemplateTypeList ||
                                    sym == gs.symBaseClassList || sym == gs.symType)
                                {
                                    pref = ContainerContextPreference::PreferClass;
                                    return false;
                                }
                                if (sym == gs.symUsingDeclaration)
                                {
                                    pref = ContainerContextPreference::PreferNamespace;
                                    return false;
                                }
                                return sym == gs.symScopedIdentifier || sym == gs.symScope;
                            });
    return pref;
}

void DisambiguateTypeContextSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    if (symbols.empty() || CheckContextPreference(ctx.node) != ContainerContextPreference::PreferClass)
    {
        return;
    }

    if (!std::any_of(symbols.begin(), symbols.end(), IsTypeSymbol))
    {
        PromoteConstructorsToTypes(ctx, symbols);
    }

    if (!std::any_of(symbols.begin(), symbols.end(), IsTypeSymbol))
    {
        return;
    }

    std::erase_if(symbols,
                  [](const analysis::Symbol& s)
                  {
                      return s.type == analysis::SymbolType::Function || s.type == analysis::SymbolType::Variable ||
                             s.type == analysis::SymbolType::Property || s.type == analysis::SymbolType::CallReference;
                  });

    auto it =
        std::find_if(symbols.begin(), symbols.end(), [](const analysis::Symbol& s)
                     { return s.type == analysis::SymbolType::Class || s.type == analysis::SymbolType::Interface; });
    if (it != symbols.end() && it != symbols.begin())
    {
        std::rotate(symbols.begin(), it, it + 1);
    }
}

void DisambiguateContainerSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    if (symbols.size() < 2)
    {
        return;
    }
    auto classIt = std::find_if(symbols.begin(), symbols.end(),
                                [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
    auto nsIt = std::find_if(symbols.begin(), symbols.end(),
                             [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
    if (classIt == symbols.end() || nsIt == symbols.end() || classIt->name != nsIt->name)
    {
        return;
    }

    auto pref = CheckContextPreference(ctx.node);
    if (pref == ContainerContextPreference::PreferClass)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
        return;
    }
    if (pref == ContainerContextPreference::PreferNamespace)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
        return;
    }

    TSNode memberNode = FindNextScopedMemberNode(ctx.parent, ctx.node);
    if (ts_node_is_null(memberNode))
    {
        return;
    }

    std::string memberName = parser::GetNodeText(memberNode, ctx.request.sourceCode);
    const bool inClass = MemberBelongsToClass(*classIt, memberName, ctx.request.symbolTable);
    const bool inNs = MemberBelongsToNamespace(*nsIt, memberName, ctx.request.symbolTable);
    if (inClass && !inNs)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
    }
    else if (inNs && !inClass)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
    }
}

void AppendConstructorSymbolsIfCall(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    TSNode callNode = FindEnclosingCallNode(ctx.node);
    if (ts_node_is_null(callNode))
    {
        return;
    }

    std::vector<analysis::Symbol> ctorSymbols;
    for (const auto& sym : symbols)
    {
        if (sym.type == analysis::SymbolType::Class)
        {
            CollectClassConstructors(sym, ctx, ctorSymbols);
        }
    }
    if (!ctorSymbols.empty())
    {
        auto best = ResolveCallOverload(ctx.node, ctorSymbols, ctx.request, ctx.scope);
        if (best.has_value())
        {
            auto it = std::find_if(ctorSymbols.begin(), ctorSymbols.end(), [&](const analysis::Symbol& s)
                                   { return s.name == best->name && analysis::HasSameParameterList(s, *best); });
            if (it != ctorSymbols.end() && it != ctorSymbols.begin())
            {
                std::rotate(ctorSymbols.begin(), it, it + 1);
            }
            symbols = std::move(ctorSymbols);
        }
        else
        {
            for (auto&& cs : ctorSymbols)
            {
                symbols.push_back(std::move(cs));
            }
        }
    }
}

void FilterDirectRootGlobalSymbols(std::vector<analysis::Symbol>& symbols, const parser::GlobalScopeQualifier& gsq)
{
    if (gsq.isGlobal && gsq.isDirectRoot)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s)
                      { return !s.containerName.empty() || s.type == analysis::SymbolType::CallReference; });
    }
}
} // namespace angel_lsp::features
