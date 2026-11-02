#include "features/hover/HoverSymbolFormatter.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ConversionRankingEngine.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "analysis/types/VirtualMixinContext.h"
#include "config/EngineRuleConfig.h"
#include "features/hover/HoverDocumentationRenderer.h"
#include "features/hover/HoverTypeInspector.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/Constants.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Timer.h"
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

/**
 * @brief Locates the local definition whose name spans the specified position.
 * @param[in] scope Starting lexical scope.
 * @param[in] position Query position.
 * @return Matching LocalDefinition pointer, or nullptr if not on declaration name.
 */
static const analysis::LocalDefinition* DefinitionAtPosition(const analysis::Scope* scope,
                                                             const lsp::Position& position)
{
    analysis::ScopeTraversalGuard cycleGuard;
    for (const analysis::Scope* current = scope; current != nullptr; current = current->parent)
    {
        if (!cycleGuard.CheckAndInsert(current))
        {
            break;
        }
        for (const analysis::LocalDefinition& def : current->definitions)
        {
            const bool afterStart = position.line > def.startLine ||
                                    (position.line == def.startLine && position.character >= def.startCharacter);
            const bool beforeEnd =
                position.line < def.endLine || (position.line == def.endLine && position.character <= def.endCharacter);

            if (afterStart && beforeEnd)
            {
                return &def;
            }
        }
    }

    return nullptr;
}

/**
 * @brief Tests whether an AST node is the member field of a member_expression parent.
 * @param[in] node Child AST node.
 * @param[in] parent Enclosing parent node.
 * @return True if node is member child, false otherwise.
 */
static bool IsMemberChildOfExpression(TSNode node, TSNode parent)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symMemberExpression)
    {
        return false;
    }
    TSNode memNode = parser::GetChildByField(parent, gs.fieldMember);
    return !ts_node_is_null(memNode) &&
           (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node));
}

/**
 * @brief Resolves receiver type name of an object expression node.
 * @param[in] objectNode Object expression TSNode.
 * @param[in] ctx Hover query context.
 * @return Qualified receiver type name or empty string.
 */
static std::string ResolveReceiverTypeName(TSNode objectNode, const HoverQueryContext& ctx)
{
    const config::EngineRuleConfig* engineRules = ctx.request.config ? &ctx.request.config->engineRules : nullptr;
    std::string receiverTypeName =
        analysis::ResolveReceiverType(objectNode, ctx.request.sourceCode, ctx.request.symbolTable,
                                      {ctx.scope, ctx.vctx.hostClass, ctx.request.uri,
                                       ctx.request.symbolTable.GetDefaultArrayType(), nullptr, engineRules});
    if (!receiverTypeName.empty() && ctx.request.config && !ctx.request.config->types.arrayTypeName.empty())
    {
        receiverTypeName = analysis::MemberOwnerType(receiverTypeName, ctx.request.config->types.arrayTypeName);
    }

    if (!receiverTypeName.empty())
    {
        receiverTypeName = ctx.request.symbolTable.QualifyShortTypeName(receiverTypeName);
    }
    return receiverTypeName;
}

/**
 * @brief Collects all member symbols matching name across receiver type hierarchy.
 * @param[in] receiverTypeName Base container type name.
 * @param[in] memberName Member identifier text.
 * @param[in] symbolTable Workspace symbol table.
 * @return Member symbols list with deduplicated overloads.
 */
static std::vector<analysis::Symbol> CollectReceiverMemberSymbols(const std::string& receiverTypeName,
                                                                  std::string_view memberName,
                                                                  const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> memberSymbols;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, symbolTable);
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedMember = utils::JoinScopedName(typeName, memberName);
        auto found = symbolTable.FindSymbolsPtr(qualifiedMember);
        if (found)
        {
            for (const auto& sym : *found)
            {
                if (sym.type == analysis::SymbolType::Function)
                {
                    bool overriddenLower =
                        std::any_of(memberSymbols.begin(), memberSymbols.end(), [&](const analysis::Symbol& kept)
                                    { return analysis::HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
                    {
                        memberSymbols.push_back(sym);
                    }
                }
                else
                {
                    memberSymbols.push_back(sym);
                }
            }
        }
    }
    std::stable_partition(memberSymbols.begin(), memberSymbols.end(),
                          [](const analysis::Symbol& s)
                          {
                              if (s.type == analysis::SymbolType::Variable &&
                                  std::holds_alternative<analysis::VariableSignature>(s.signature))
                              {
                                  return !s.GetVariable().isEnumConstant;
                              }
                              return true;
                          });
    return memberSymbols;
}

/**
 * @brief Attempts hover resolution for a member access expression.
 * @param[in] ctx Hover query context.
 * @return Formatted member hover or std::nullopt.
 */
static std::optional<lsp::Hover> TryHoverMemberAccess(const HoverQueryContext& ctx)
{
    utils::HighResTimer symTimer;
    if (!IsMemberChildOfExpression(ctx.node, ctx.parent))
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    TSNode objectNode = parser::GetChildByField(ctx.parent, parser::GetGrammarSymbols().fieldObject);
    if (ts_node_is_null(objectNode))
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    std::string receiverTypeName = ResolveReceiverTypeName(objectNode, ctx);
    if (receiverTypeName.empty())
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    std::vector<analysis::Symbol> memberSymbols =
        CollectReceiverMemberSymbols(receiverTypeName, ctx.nodeText, ctx.request.symbolTable);

    std::string accessorPropertyType;
    if (memberSymbols.empty())
    {
        const int accessorMode = ctx.request.config ? ctx.request.config->engine.propertyAccessorMode : 2;
        if (accessorMode >= 2)
        {
            memberSymbols = analysis::FindPropertyAccessors(receiverTypeName, ctx.nodeText, ctx.request.symbolTable,
                                                            accessorMode == 3);
            accessorPropertyType = analysis::PropertyTypeFromAccessors(memberSymbols);
        }
    }

    if (memberSymbols.empty())
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    if (ctx.profiler.IsActive())
    {
        ctx.profiler.pathText = receiverTypeName + "." + ctx.nodeText;
    }
    ctx.profiler.symMs += symTimer.ElapsedMs();
    return FormatMemberHover(memberSymbols, accessorPropertyType, ctx);
}

/**
 * @brief Attempts hover resolution for in-scope local definitions.
 * @param[in] ctx Hover query context.
 * @return Formatted hover response or std::nullopt.
 */
static std::optional<lsp::Hover> TryHoverLocalDefinition(const HoverQueryContext& ctx)
{
    if (!ctx.rootScope || !ctx.scope)
    {
        return std::nullopt;
    }

    utils::HighResTimer symTimer;
    lsp::Position queryPos{ctx.queryLine, ctx.request.position.character};
    const analysis::LocalDefinition* def = DefinitionAtPosition(ctx.scope, queryPos);
    if (!def)
    {
        uint32_t cursorByte = ts_node_start_byte(ctx.node);
        def = analysis::ResolveInScopeAt(ctx.scope, ctx.nodeText, cursorByte);
    }

    if (!def ||
        (def->kind != analysis::LocalDefinitionKind::Parameter && def->kind != analysis::LocalDefinitionKind::Variable))
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    const std::string deducedType = DeduceLocalDefinitionHoverType(*def, ctx);
    const std::string_view typeName = deducedType;

    if (ctx.profiler.IsActive())
    {
        ctx.profiler.symbolName = def->name;
    }
    ctx.profiler.symMs += symTimer.ElapsedMs();
    utils::HighResTimer fmtTimer;
    std::string md;
    md.reserve(128);
    md += "```angelscript\n";

    if (def->kind == analysis::LocalDefinitionKind::Parameter)
    {
        FormatParameterHover(*def, typeName, ctx, md);
    }
    else
    {
        FormatVariableHover(*def, typeName, ctx, md);
    }
    md += "\n```";

    AppendLocalDoc(*def, typeName, ctx, md);

    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, ctx.range};
}

/**
 * @brief Fallback hover resolution for parameter AST nodes without scope tree entries.
 * @param[in] ctx Hover query context.
 * @return Parameter hover if node is part of a parameter declaration, std::nullopt otherwise.
 */
static std::optional<lsp::Hover> TryHoverParameterAst(const HoverQueryContext& ctx)
{
    if (ts_node_is_null(ctx.node) || !ctx.request.tree)
    {
        return std::nullopt;
    }

    TSNode paramNode = parser::FindAncestorWithSymbol(ctx.node, parser::GetGrammarSymbols().symParameter);
    if (ts_node_is_null(paramNode))
    {
        return std::nullopt;
    }

    TSNode rootNode = ts_tree_root_node(ctx.request.tree);
    std::string declText = ExtractDeclarationTextAt(rootNode, ctx.request.sourceCode, ts_node_start_point(ctx.node),
                                                    parser::GetGrammarSymbols().symParameter);
    if (declText.empty())
    {
        return std::nullopt;
    }

    std::string md = "```angelscript\n(parameter) " + declText + "\n```";
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(md)}, ctx.range};
}

static bool IsNamedArgumentLabelAtCursor(TSNode node, TSNode parent)
{
    if (ts_node_is_null(node) || ts_node_is_null(parent) ||
        ts_node_symbol(parent) != parser::GetGrammarSymbols().symArgumentList)
    {
        return false;
    }
    TSNode next = ts_node_next_sibling(node);
    while (!ts_node_is_null(next) && ts_node_symbol(next) == parser::GetGrammarSymbols().symComment)
    {
        next = ts_node_next_sibling(next);
    }
    return !ts_node_is_null(next) && ts_node_symbol(next) == parser::GetGrammarSymbols().tokColon;
}

static std::vector<analysis::Symbol> ResolveNamedArgCalleeCandidates(TSNode callNode, const HoverQueryContext& ctx)
{
    TSNode funcNode = parser::GetChildByField(callNode, parser::GetGrammarSymbols().fieldFunction);
    if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
    {
        funcNode = ts_node_child(callNode, 0);
    }
    if (ts_node_is_null(funcNode))
    {
        return {};
    }

    if (ts_node_symbol(funcNode) == parser::GetGrammarSymbols().symMemberExpression)
    {
        TSNode objectNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldObject);
        TSNode memberNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldMember);
        if (!ts_node_is_null(objectNode) && !ts_node_is_null(memberNode))
        {
            std::string receiverTypeName = ResolveReceiverTypeName(objectNode, ctx);
            std::string memberName = parser::GetNodeText(memberNode, ctx.request.sourceCode);
            return CollectReceiverMemberSymbols(receiverTypeName, memberName, ctx.request.symbolTable);
        }
    }
    else
    {
        std::string funcName = parser::GetNodeText(funcNode, ctx.request.sourceCode);
        if (const auto found = ctx.request.symbolTable.FindSymbolsPtr(funcName))
        {
            return *found;
        }
    }
    return {};
}

static std::optional<lsp::Hover> TryHoverNamedArgument(const HoverQueryContext& ctx)
{
    if (!IsNamedArgumentLabelAtCursor(ctx.node, ctx.parent))
    {
        return std::nullopt;
    }

    TSNode callNode = parser::GetParent(ctx.parent);
    if (ts_node_is_null(callNode))
    {
        return std::nullopt;
    }

    auto candidates = ResolveNamedArgCalleeCandidates(callNode, ctx);
    return FormatNamedArgHover(candidates, ctx);
}

std::optional<lsp::Hover> TryHoverExpressionOrLocal(const HoverQueryContext& ctx)
{
    if (auto autoHover = TryHoverAutoType(ctx))
    {
        return autoHover;
    }

    if (IsMemberChildOfExpression(ctx.node, ctx.parent))
    {
        if (auto memberHover = TryHoverMemberAccess(ctx))
        {
            return memberHover;
        }
    }

    if (auto localHover = TryHoverLocalDefinition(ctx))
    {
        return localHover;
    }

    if (auto paramHover = TryHoverParameterAst(ctx))
    {
        return paramHover;
    }

    if (auto namedArgHover = TryHoverNamedArgument(ctx))
    {
        return namedArgHover;
    }

    return std::nullopt;
}

static bool IsQualifiedMemberNode(TSNode node, std::string_view sourceCode = {})
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pType = ts_node_symbol(parent);
    if (pType == gs.symScopedIdentifier)
    {
        TSNode firstChild = ts_node_named_child(parent, 0);
        return !ts_node_is_null(firstChild) && !ts_node_eq(firstChild, node) &&
               ts_node_start_byte(firstChild) != ts_node_start_byte(node);
    }
    if (pType == gs.symMemberExpression)
    {
        TSNode memberField = parser::GetChildByField(parent, gs.fieldMember);
        return ts_node_eq(memberField, node) ||
               (!ts_node_is_null(memberField) && ts_node_start_byte(memberField) == ts_node_start_byte(node));
    }
    if (pType == gs.symDatatype && !sourceCode.empty())
    {
        return !analysis::ExtractDatatypeScopePrefix(parent, sourceCode).empty();
    }
    return false;
}

static void AppendEnclosingClassMethods(TSNode node, std::string_view nodeText, const HoverRequest& request,
                                        std::vector<analysis::Symbol>& symbols)
{
    auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
        {
            auto hierarchy = analysis::GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName,
                                                                 request.symbolTable);
            for (const auto& typeName : hierarchy)
            {
                auto found = request.symbolTable.FindMemberSymbolPtr(typeName, nodeText);
                if (found)
                {
                    for (const auto& sym : *found)
                    {
                        if (sym.type == analysis::SymbolType::Function &&
                            sym.containerKind != analysis::ContainerKind::Namespace)
                        {
                            bool overriddenLower =
                                std::any_of(symbols.begin(), symbols.end(), [&](const analysis::Symbol& kept)
                                            { return analysis::HasSameParameterList(kept, sym); });
                            if (!overriddenLower)
                            {
                                symbols.push_back(sym);
                            }
                        }
                    }
                }
            }
            break;
        }
    }
}

static std::vector<analysis::Symbol> CollectHierarchyHostSymbols(const std::string& hostClass,
                                                                 std::string_view nodeText,
                                                                 const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> hostSymbols;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(hostClass, symbolTable);
    for (const auto& typeName : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbolPtr(typeName, nodeText);
        if (found)
        {
            for (const auto& sym : *found)
            {
                if (sym.containerKind == analysis::ContainerKind::Namespace)
                {
                    continue;
                }
                if (sym.type == analysis::SymbolType::Function)
                {
                    bool overriddenLower =
                        std::any_of(hostSymbols.begin(), hostSymbols.end(), [&](const analysis::Symbol& kept)
                                    { return analysis::HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
                    {
                        hostSymbols.push_back(sym);
                    }
                }
                else if (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property)
                {
                    hostSymbols.push_back(sym);
                }
            }
        }
    }
    return hostSymbols;
}

static void MergeHostSymbols(std::vector<analysis::Symbol>& symbols, std::vector<analysis::Symbol> hostSymbols,
                             const std::string& fileUri)
{
    std::vector<analysis::Symbol> containerSymbols;
    for (const auto& s : symbols)
    {
        if (!s.containerName.empty() || s.fileUri == fileUri)
        {
            containerSymbols.push_back(s);
        }
    }
    symbols = std::move(containerSymbols);

    for (auto& hs : hostSymbols)
    {
        bool present = std::any_of(symbols.begin(), symbols.end(), [&](const analysis::Symbol& s)
                                   { return s.name == hs.name && analysis::HasSameParameterList(s, hs); });
        if (!present)
        {
            symbols.push_back(std::move(hs));
        }
    }
}

static void AppendVirtualHostSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols,
                                     std::string& accessorPropertyType)
{
    std::string hostClass;
    if (ctx.vctx.isVirtual && !ctx.vctx.hostClass.empty())
    {
        hostClass = ctx.vctx.hostClass;
    }
    else
    {
        hostClass =
            analysis::InferHostClassFromEnclosingMixin(ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    }
    if (hostClass.empty())
    {
        return;
    }

    std::vector<analysis::Symbol> hostSymbols =
        CollectHierarchyHostSymbols(hostClass, ctx.nodeText, ctx.request.symbolTable);

    if (hostSymbols.empty())
    {
        auto hierarchy = analysis::GetInheritedTypeHierarchy(hostClass, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            auto accessors = analysis::FindPropertyAccessors(typeName, ctx.nodeText, ctx.request.symbolTable, false);
            if (!accessors.empty())
            {
                hostSymbols = std::move(accessors);
                accessorPropertyType = analysis::PropertyTypeFromAccessors(hostSymbols);
                break;
            }
        }
    }

    if (!hostSymbols.empty())
    {
        MergeHostSymbols(symbols, std::move(hostSymbols), ctx.request.uri);
    }
}

static std::optional<lsp::Hover> TryHoverScopeFallback(const HoverQueryContext& ctx)
{
    if (!ctx.scope)
    {
        return std::nullopt;
    }

    uint32_t cursorByte = ts_node_start_byte(ctx.node);
    const analysis::LocalDefinition* def = analysis::ResolveInScopeAt(ctx.scope, ctx.nodeText, cursorByte);
    if (!def)
    {
        return std::nullopt;
    }

    utils::HighResTimer fmtTimer;
    std::ostringstream oss;
    oss << "```angelscript\n";
    if (def->kind == analysis::LocalDefinitionKind::Field)
    {
        oss << "(field) ";
    }
    if (!def->typeName.empty())
    {
        oss << def->typeName << " ";
    }
    oss << def->name << "\n```";
    std::string doc = analysis::ExtractDocComment(ctx.request.sourceCode, def->startLine);
    if (!doc.empty())
    {
        oss << "\n\n---\n\n" << doc;
    }
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), oss.str()}, ctx.range};
}

static void FilterEnumMemberSymbols(std::vector<analysis::Symbol>& symbols, const HoverQueryContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(ctx.parent) || ts_node_symbol(ctx.parent) != gs.symEnumMember)
    {
        return;
    }
    TSNode enumDecl = parser::GetParent(ctx.parent);
    if (ts_node_is_null(enumDecl) || ts_node_symbol(enumDecl) != gs.symEnumDeclaration)
    {
        return;
    }
    TSNode nameNode = parser::GetChildByField(enumDecl, gs.fieldName);
    if (ts_node_is_null(nameNode))
    {
        return;
    }
    std::string enumName = parser::GetNodeText(nameNode, ctx.request.sourceCode);
    if (enumName.empty())
    {
        return;
    }
    std::erase_if(symbols,
                  [&](const analysis::Symbol& s)
                  {
                      if (s.containerName == enumName || utils::EndsWithScopedMember(s.containerName, enumName))
                      {
                          return false;
                      }
                      if (std::holds_alternative<analysis::VariableSignature>(s.signature))
                      {
                          const auto& vs = s.GetVariable();
                          if (vs.isEnumConstant &&
                              (vs.typeName == enumName || utils::EndsWithScopedMember(vs.typeName, enumName)))
                          {
                              return false;
                          }
                      }
                      return true;
                  });
}

static void ResolveScopedMemberFallback(std::string_view scopedPrefix, const HoverQueryContext& ctx,
                                        std::vector<analysis::Symbol>& symbols)
{
    if (!symbols.empty() || !utils::ContainsScopeSeparator(scopedPrefix))
    {
        return;
    }
    const std::string_view qualifierView = utils::ParentScopeView(scopedPrefix);
    if (qualifierView.empty())
    {
        return;
    }
    const std::string qualifier(qualifierView);
    auto qualSyms = analysis::FindSymbolsInScope(qualifier, ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    for (const auto& qs : qualSyms)
    {
        const std::string qName = qs.qualifiedName.empty() ? qs.name : qs.qualifiedName;
        if (const auto found = ctx.request.symbolTable.FindMemberSymbolPtr(qName, ctx.nodeText))
        {
            symbols.insert(symbols.end(), found->begin(), found->end());
            return;
        }
    }
}

static std::vector<analysis::Symbol> CollectScopedSymbols(HoverQueryContext& ctx)
{
    std::vector<analysis::Symbol> symbols;
    bool isExplicitlyScoped = false;
    if (!ts_node_is_null(ctx.parent))
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol pSym = ts_node_symbol(ctx.parent);
        if (pSym == gs.symScopedIdentifier)
        {
            const uint32_t pStart = ts_node_start_byte(ctx.parent);
            const uint32_t nEnd = ts_node_end_byte(ctx.node);
            if (pStart < ctx.request.sourceCode.size() && nEnd <= ctx.request.sourceCode.size() && pStart < nEnd)
            {
                const std::string scopedPrefix = ctx.request.sourceCode.substr(pStart, nEnd - pStart);
                if (ctx.profiler.IsActive())
                {
                    ctx.profiler.pathText = scopedPrefix;
                }
                symbols = analysis::FindSymbolsInScope(scopedPrefix, ctx.node, ctx.request.sourceCode,
                                                       ctx.request.symbolTable);
                ResolveScopedMemberFallback(scopedPrefix, ctx, symbols);
                isExplicitlyScoped = utils::ContainsScopeSeparator(scopedPrefix);
            }
        }
        else if (pSym == gs.symDatatype)
        {
            const std::string prefix = analysis::ExtractDatatypeScopePrefix(ctx.parent, ctx.request.sourceCode);
            if (!prefix.empty())
            {
                if (ctx.profiler.IsActive())
                {
                    ctx.profiler.pathText = prefix + ctx.nodeText;
                }
                symbols = analysis::FindSymbolsInScope(prefix + ctx.nodeText, ctx.node, ctx.request.sourceCode,
                                                       ctx.request.symbolTable);
                isExplicitlyScoped = true;
            }
        }
    }
    if (symbols.empty() && !isExplicitlyScoped)
    {
        symbols = analysis::FindSymbolsInScope(ctx.nodeText, ctx.node, ctx.request.sourceCode, ctx.request.symbolTable);
    }
    FilterEnumMemberSymbols(symbols, ctx);
    std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::CallReference; });
    return symbols;
}

static void PopulateGlobalPropertyAccessors(std::vector<analysis::Symbol>& symbols, std::string& accessorPropertyType,
                                            const HoverQueryContext& ctx)
{
    const int accessorMode = ctx.request.config ? ctx.request.config->engine.propertyAccessorMode : 2;
    if (accessorMode >= 2)
    {
        symbols = analysis::FindGlobalPropertyAccessors(ctx.nodeText, ctx.request.symbolTable, accessorMode == 3);
        accessorPropertyType = analysis::PropertyTypeFromAccessors(symbols);
    }
}

static std::optional<lsp::Hover> TryHoverDelegatePropertyFallback(const HoverQueryContext& ctx)
{
    const auto* engineRules = ctx.request.config ? &ctx.request.config->engineRules : nullptr;
    if (!analysis::IsBaseClassDelegateProperty(ctx.nodeText, ctx.request.symbolTable, engineRules))
    {
        return std::nullopt;
    }
    std::string baseClass =
        analysis::ResolveDelegateTargetBaseClass({ctx.node, ctx.request.sourceCode, ctx.request.symbolTable,
                                                  ctx.vctx.isVirtual ? ctx.vctx.hostClass : "", engineRules});
    if (!baseClass.empty())
    {
        std::string text = "```angelscript\n" + baseClass + "@ " + ctx.nodeText + "\n```";
        return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(text)},
                          ctx.range};
    }
    return std::nullopt;
}

std::optional<lsp::Hover> TryHoverSymbolCandidates(HoverQueryContext& ctx)
{
    utils::HighResTimer symTimer;
    const auto gsq = parser::GetGlobalScopeQualifier(ctx.node, ctx.request.sourceCode);
    std::vector<analysis::Symbol> symbols = CollectScopedSymbols(ctx);
    if (!gsq.isGlobal && !IsQualifiedMemberNode(ctx.node, ctx.request.sourceCode))
    {
        AppendEnclosingClassMethods(ctx.node, ctx.nodeText, ctx.request, symbols);
    }
    DisambiguateContainerSymbols(ctx, symbols);
    DisambiguateTypeContextSymbols(ctx, symbols);
    AppendConstructorSymbolsIfCall(ctx, symbols);

    std::string accessorPropertyType;
    if (!gsq.isGlobal)
    {
        AppendVirtualHostSymbols(ctx, symbols, accessorPropertyType);
    }

    FilterDirectRootGlobalSymbols(symbols, gsq);

    if (symbols.empty() && ctx.rootScope && !gsq.isGlobal)
    {
        if (auto fallbackHover = TryHoverScopeFallback(ctx))
        {
            ctx.profiler.symMs += symTimer.ElapsedMs();
            return fallbackHover;
        }
    }

    if (symbols.empty() && accessorPropertyType.empty() && !gsq.isGlobal)
    {
        PopulateGlobalPropertyAccessors(symbols, accessorPropertyType, ctx);
    }

    if (symbols.empty() && !gsq.isGlobal)
    {
        if (auto specialHover = TryHoverDelegatePropertyFallback(ctx))
        {
            return specialHover;
        }
    }

    ctx.profiler.symMs += symTimer.ElapsedMs();
    if (symbols.empty())
    {
        return std::nullopt;
    }

    return FormatSymbolsHover(symbols, accessorPropertyType, ctx);
}
} // namespace angel_lsp::features
