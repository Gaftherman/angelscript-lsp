#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
namespace
{
using analysis::Symbol;
using analysis::SymbolTable;
using analysis::SymbolType;
using analysis::rules::RuleIndex;

bool IsTypeSymbol(const Symbol& sym)
{
    return sym.type == SymbolType::Class || sym.type == SymbolType::Interface;
}

/** @brief Every base a declaration lists, whichever kind of declaration it is. */
std::vector<std::string> DeclaredBases(const Symbol& sym)
{
    if (sym.type == SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(sym.signature))
    {
        return sym.GetClass().bases;
    }
    if (sym.type == SymbolType::Interface && std::holds_alternative<analysis::InterfaceSignature>(sym.signature))
    {
        return sym.GetInterface().inheritedInterfaces;
    }
    return {};
}

lsp::Range ToRange(const analysis::SourceRange& range)
{
    return lsp::Range{lsp::Position{range.startLine, range.startCharacter},
                      lsp::Position{range.endLine, range.endCharacter}};
}

lsp::TypeHierarchyItem ToItem(const Symbol& sym)
{
    lsp::TypeHierarchyItem item;
    item.name = analysis::LastScopeSegment(sym.name);
    item.kind = sym.type == SymbolType::Interface ? lsp::SymbolKind::Interface : lsp::SymbolKind::Class;
    item.uri = lsp::DocumentUri::parse(sym.fileUri);
    std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    item.data = lsp::json::Value(std::move(qName));
    if (!sym.containerName.empty())
    {
        item.detail = sym.containerName;
    }

    // fullRange covers the declaration and its body; selectionRange is the name alone. The
    // protocol requires the second be contained by the first, and a collector that recorded
    // neither would give both as an empty range at the origin - so the symbol's own
    // start/end is the fallback, which is always at least the name.
    const bool hasFullRange = sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0;
    item.range = hasFullRange ? ToRange(sym.fullRange)
                              : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                           lsp::Position{sym.endLine, sym.endCharacter}};

    const bool hasSelection = sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0;
    item.selectionRange = hasSelection ? ToRange(sym.selectionRange)
                                       : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                                    lsp::Position{sym.endLine, sym.endCharacter}};
    return item;
}

/** @brief The type declarations a bare name resolves to, if any. */
std::vector<Symbol> FindTypeDeclarations(const std::string& name, const SymbolTable& table)
{
    std::vector<Symbol> found;
    const auto symbols = table.FindSymbolsPtr(name);
    if (!symbols)
    {
        return found;
    }
    for (const auto& sym : *symbols)
    {
        if (IsTypeSymbol(sym))
        {
            found.push_back(sym);
        }
    }
    return found;
}

/** @brief The identifier the cursor sits on, or empty when it is not on one. */
std::string IdentifierAt(const TypeHierarchyPrepareRequest& request, TSNode& outNode)
{
    outNode = TSNode{};
    if (!request.tree)
    {
        return "";
    }

    const TSNode root = ts_tree_root_node(request.tree);
    const TSPoint point{request.position.line, request.position.character};
    TSNode node = ts_node_descendant_for_point_range(root, point, point);
    if (ts_node_is_null(node))
    {
        return "";
    }

    outNode = node;
    if (std::string_view(ts_node_type(node)) != "identifier")
    {
        return "";
    }

    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > request.sourceCode.size())
    {
        return "";
    }
    return request.sourceCode.substr(start, end - start);
}

/**
 * @brief Attempts to find type declarations from a qualified type under the cursor.
 * @param[in] node AST node at cursor.
 * @param[in] sourceCode Document source text.
 * @param[in] symbolTable Symbol table to look up types.
 * @return List of matching type symbols if found.
 */
std::vector<Symbol> FindQualifiedTypeUnderCursor(TSNode node, std::string_view sourceCode,
                                                 const SymbolTable& symbolTable)
{
    std::vector<Symbol> declarations;
    TSNode p = node;
    while (!ts_node_is_null(p))
    {
        const std::string_view pType = ts_node_type(p);
        if (pType == "type" || pType == "scoped_identifier")
        {
            const std::string text = analysis::CleanBaseType(analysis::GetNodeText(p, sourceCode));
            if (!text.empty() && text.find("::") != std::string::npos)
            {
                declarations = FindTypeDeclarations(text, symbolTable);
                if (declarations.empty())
                {
                    for (const auto& container : analysis::GetEnclosingContainers(p, sourceCode))
                    {
                        if (container.kind == analysis::ContainerKind::Namespace)
                        {
                            declarations = FindTypeDeclarations(container.qualifiedName + "::" + text, symbolTable);
                            if (!declarations.empty())
                            {
                                break;
                            }
                        }
                    }
                }
                if (!declarations.empty())
                {
                    break;
                }
            }
        }
        if (pType == "class_declaration" || pType == "interface_declaration" || pType == "func_declaration" ||
            pType == "statement_block")
        {
            break;
        }
        p = ts_node_parent(p);
    }
    return declarations;
}

/**
 * @brief Resolves type declarations for an identifier name, checking namespaces and short names.
 * @param[in] name Identifier name.
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @param[in] symbolTable Symbol table.
 * @return List of matching type symbols.
 */
std::vector<Symbol> FindTypeFromIdentifier(std::string_view name, TSNode node, std::string_view sourceCode,
                                           const SymbolTable& symbolTable)
{
    const std::string nameStr(name);
    auto declarations = FindTypeDeclarations(nameStr, symbolTable);
    if (declarations.empty() && !ts_node_is_null(node))
    {
        for (const auto& container : analysis::GetEnclosingContainers(node, sourceCode))
        {
            if (container.kind == analysis::ContainerKind::Namespace)
            {
                declarations = FindTypeDeclarations(container.qualifiedName + "::" + nameStr, symbolTable);
                if (!declarations.empty())
                {
                    break;
                }
            }
        }
    }
    if (declarations.empty())
    {
        for (const auto& sym : symbolTable.FindTypeSymbolsByShortName(nameStr))
        {
            if (IsTypeSymbol(sym))
            {
                declarations.push_back(sym);
            }
        }
    }
    return declarations;
}

/**
 * @brief Resolves the enclosing class or interface type containing the cursor node.
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @param[in] symbolTable Symbol table.
 * @return List of matching type symbols.
 */
std::vector<Symbol> FindEnclosingType(TSNode node, std::string_view sourceCode, const SymbolTable& symbolTable)
{
    std::vector<Symbol> declarations;
    for (const auto& container : analysis::GetEnclosingContainers(node, sourceCode))
    {
        if (container.kind != analysis::ContainerKind::Class && container.kind != analysis::ContainerKind::Interface)
        {
            continue;
        }
        declarations = FindTypeDeclarations(container.qualifiedName, symbolTable);
        if (declarations.empty())
        {
            declarations = FindTypeDeclarations(container.name, symbolTable);
        }
        if (declarations.empty())
        {
            for (const auto& sym : symbolTable.FindTypeSymbolsByShortName(container.name))
            {
                if (IsTypeSymbol(sym))
                {
                    declarations.push_back(sym);
                }
            }
        }
        break;
    }
    return declarations;
}

/**
 * @brief Computes the effective range of a symbol, preferring selectionRange, then fullRange, then start/end.
 * @param[in] sym Target symbol.
 * @return Effective LSP Range.
 */
lsp::Range GetSymbolEffectiveRange(const Symbol& sym)
{
    if (sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0)
    {
        return ToRange(sym.selectionRange);
    }
    if (sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0)
    {
        return ToRange(sym.fullRange);
    }
    return lsp::Range{lsp::Position{sym.startLine, sym.startCharacter}, lsp::Position{sym.endLine, sym.endCharacter}};
}

/**
 * @brief Checks if a candidate symbol matches the file URI and selection line of an item.
 * @param[in] sym Candidate symbol.
 * @param[in] item Target type hierarchy item.
 * @param[in] itemUri Serialized document URI of the item.
 * @return True if the symbol matches.
 */
bool MatchesItemLocation(const Symbol& sym, const lsp::TypeHierarchyItem& item, const std::string& itemUri)
{
    if (sym.fileUri != itemUri && angel_lsp::utils::PathToUri(sym.fileUri) != itemUri)
    {
        return false;
    }
    return GetSymbolEffectiveRange(sym).start.line == item.selectionRange.start.line;
}

/**
 * @brief Resolves type declarations for an LSP TypeHierarchyItem, filtering by URI/range if needed.
 * @param[in] item Target type hierarchy item.
 * @param[in] symbolTable Symbol table.
 * @return Filtered declaration symbols.
 */
std::vector<Symbol> ResolveItemDeclarations(const lsp::TypeHierarchyItem& item, const SymbolTable& symbolTable)
{
    std::vector<Symbol> declarations;
    if (item.data.has_value() && item.data->isString() && !item.data->string().empty())
    {
        declarations = FindTypeDeclarations(item.data->string(), symbolTable);
    }
    if (declarations.empty())
    {
        declarations = FindTypeDeclarations(item.name, symbolTable);
    }
    if (declarations.empty())
    {
        for (const auto& sym : symbolTable.FindTypeSymbolsByShortName(item.name))
        {
            if (IsTypeSymbol(sym))
            {
                declarations.push_back(sym);
            }
        }
    }
    if (declarations.size() > 1 && item.uri.isValid())
    {
        const std::string itemUri = item.uri.toString();
        auto it = std::find_if(declarations.begin(), declarations.end(),
                               [&](const Symbol& sym) { return MatchesItemLocation(sym, item, itemUri); });
        if (it != declarations.end())
        {
            declarations = {*it};
        }
    }
    return declarations;
}

/**
 * @brief Resolves candidate base type symbols in search order.
 * @param[in] cleanBase Normalized base type name.
 * @param[in] declPrefix Namespace prefix of the declaring type.
 * @param[in] symbolTable Symbol table.
 * @return Resolved base type symbols.
 */
std::vector<Symbol> ResolveBaseSymbols(const std::string& cleanBase, std::string_view declPrefix,
                                       const SymbolTable& symbolTable)
{
    std::vector<Symbol> baseSymbols;
    if (cleanBase.find("::") != std::string::npos)
    {
        baseSymbols = FindTypeDeclarations(cleanBase, symbolTable);
    }
    if (baseSymbols.empty() && !declPrefix.empty())
    {
        baseSymbols = FindTypeDeclarations(std::string(declPrefix) + "::" + cleanBase, symbolTable);
    }
    const std::string baseName = analysis::LastScopeSegment(cleanBase);
    if (baseSymbols.empty() && !baseName.empty())
    {
        baseSymbols = FindTypeDeclarations(baseName, symbolTable);
    }
    if (baseSymbols.empty() && !baseName.empty())
    {
        for (const auto& sym : symbolTable.FindTypeSymbolsByShortName(baseName))
        {
            if (IsTypeSymbol(sym))
            {
                baseSymbols.push_back(sym);
            }
        }
    }
    return baseSymbols;
}

/**
 * @brief Collects supertype items for a single declaration, tracking seen types to avoid duplicates.
 * @param[in] declaration Declaring type symbol.
 * @param[in] symbolTable Symbol table.
 * @param[in,out] seen Set of already emitted type keys.
 * @param[in,out] items Output list of supertype items.
 */
void CollectSupertypesForDeclaration(const Symbol& declaration, const SymbolTable& symbolTable,
                                     std::vector<std::string>& seen, std::vector<lsp::TypeHierarchyItem>& items)
{
    std::string declPrefix;
    const auto lastScope = declaration.name.rfind("::");
    if (lastScope != std::string::npos)
    {
        declPrefix = declaration.name.substr(0, lastScope);
    }

    for (const auto& base : DeclaredBases(declaration))
    {
        const std::string cleanBase = analysis::CleanBaseType(base);
        if (cleanBase.empty())
        {
            continue;
        }

        const auto baseSymbols = ResolveBaseSymbols(cleanBase, declPrefix, symbolTable);
        for (const auto& baseSymbol : baseSymbols)
        {
            const std::string key = baseSymbol.qualifiedName.empty() ? baseSymbol.name : baseSymbol.qualifiedName;
            if (std::find(seen.begin(), seen.end(), key) == seen.end())
            {
                seen.push_back(key);
                items.push_back(ToItem(baseSymbol));
            }
        }
    }
}

/**
 * @brief Context for subtype search queries.
 */
struct SubtypeQueryContext
{
    std::string target;
    std::string qualifiedTarget;
    std::string itemName;
    const SymbolTable& symbolTable;
};

/**
 * @brief Queries rule index for derived subtypes of target and qualifiedTarget.
 * @param[in] ruleIndex Rule index.
 * @param[in] ctx Subtype query context.
 * @param[in,out] items Output list of subtype items.
 */
void CollectSubtypesFromRuleIndex(const RuleIndex& ruleIndex, const SubtypeQueryContext& ctx,
                                  std::vector<lsp::TypeHierarchyItem>& items)
{
    ankerl::unordered_dense::set<std::string> seen;
    auto collectFrom = [&](const std::string& baseKey)
    {
        auto it = ruleIndex.derivedByBase.find(baseKey);
        if (it == ruleIndex.derivedByBase.end())
        {
            return;
        }
        for (const auto& derived : it->second)
        {
            const std::string key = derived.qualifiedName.empty() ? derived.name : derived.qualifiedName;
            if (!seen.insert(key).second)
            {
                continue;
            }
            const auto symList = ctx.symbolTable.FindSymbolsPtr(key);
            if (symList)
            {
                for (const auto& sym : *symList)
                {
                    if (IsTypeSymbol(sym) && analysis::LastScopeSegment(sym.name) != ctx.target)
                    {
                        items.push_back(ToItem(sym));
                    }
                }
            }
        }
    };

    if (!ctx.qualifiedTarget.empty())
    {
        collectFrom(ctx.qualifiedTarget);
    }
    collectFrom(ctx.target);
    if (ctx.itemName != ctx.target)
    {
        collectFrom(ctx.itemName);
    }
}

/**
 * @brief Fallback linear symbol table scan for derived subtypes.
 * @param[in] ctx Subtype query context.
 * @param[in,out] items Output list of subtype items.
 */
void CollectSubtypesByScan(const SubtypeQueryContext& ctx, std::vector<lsp::TypeHierarchyItem>& items)
{
    ctx.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (!IsTypeSymbol(sym) || analysis::LastScopeSegment(sym.name) == ctx.target)
                {
                    continue;
                }

                for (const auto& base : DeclaredBases(sym))
                {
                    const std::string cleanBase = analysis::CleanBaseType(base);
                    if ((!ctx.qualifiedTarget.empty() && cleanBase == ctx.qualifiedTarget) ||
                        analysis::LastScopeSegment(cleanBase) == ctx.target)
                    {
                        items.push_back(ToItem(sym));
                        break;
                    }
                }
            }
        });
}
} // namespace

std::optional<std::vector<lsp::TypeHierarchyItem>> PrepareTypeHierarchy(const TypeHierarchyPrepareRequest& request)
{
    TSNode node{};
    const std::string name = IdentifierAt(request, node);

    std::vector<Symbol> declarations;
    if (!ts_node_is_null(node))
    {
        declarations = FindQualifiedTypeUnderCursor(node, request.sourceCode, request.symbolTable);
    }
    if (declarations.empty() && !name.empty())
    {
        declarations = FindTypeFromIdentifier(name, node, request.sourceCode, request.symbolTable);
    }
    if (declarations.empty() && !ts_node_is_null(node))
    {
        declarations = FindEnclosingType(node, request.sourceCode, request.symbolTable);
    }

    if (declarations.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::TypeHierarchyItem> items;
    items.reserve(declarations.size());
    for (const auto& sym : declarations)
    {
        items.push_back(ToItem(sym));
    }
    return items;
}

std::optional<std::vector<lsp::TypeHierarchyItem>> GetSupertypes(const TypeHierarchyItemRequest& request)
{
    const auto declarations = ResolveItemDeclarations(request.item, request.symbolTable);
    if (declarations.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::TypeHierarchyItem> items;
    std::vector<std::string> seen;
    for (const auto& declaration : declarations)
    {
        CollectSupertypesForDeclaration(declaration, request.symbolTable, seen, items);
    }

    return items.empty() ? std::nullopt : std::optional{items};
}

std::optional<std::vector<lsp::TypeHierarchyItem>> GetSubtypes(const TypeHierarchyItemRequest& request)
{
    const std::string target = analysis::LastScopeSegment(request.item.name);
    if (target.empty())
    {
        return std::nullopt;
    }

    std::string qualifiedTarget;
    if (request.item.data.has_value() && request.item.data->isString() && !request.item.data->string().empty())
    {
        qualifiedTarget = request.item.data->string();
    }

    SubtypeQueryContext ctx{target, std::move(qualifiedTarget), request.item.name, request.symbolTable};
    std::vector<lsp::TypeHierarchyItem> items;

    const auto ruleIndex = request.symbolTable.GetRuleIndex();
    if (ruleIndex)
    {
        CollectSubtypesFromRuleIndex(*ruleIndex, ctx, items);
    }
    else
    {
        CollectSubtypesByScan(ctx, items);
    }

    return items.empty() ? std::nullopt : std::optional{items};
}
} // namespace angel_lsp::features
