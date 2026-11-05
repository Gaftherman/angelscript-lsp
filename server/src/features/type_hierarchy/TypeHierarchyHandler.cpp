#include "features/type_hierarchy/TypeHierarchyHandler.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/Constants.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

static bool IsTypeSymbol(const analysis::Symbol& sym)
{
    return sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface;
}

/**
 * @brief Converts an analysis symbol into an LSP TypeHierarchyItem.
 * @param[in] sym Analysis symbol representing a class or interface.
 * @return Constructed TypeHierarchyItem populated with symbol details.
 */
static lsp::TypeHierarchyItem ToItem(const analysis::Symbol& sym)
{
    lsp::TypeHierarchyItem item;
    item.name = std::string(analysis::LastScopeSegment(sym.name));
    item.kind = sym.type == analysis::SymbolType::Interface ? lsp::SymbolKind::Interface : lsp::SymbolKind::Class;
    item.uri = lsp::DocumentUri::parse(sym.fileUri);
    std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    item.data = lsp::json::Value(std::move(qName));
    if (!sym.containerName.empty())
    {
        item.detail = sym.containerName;
    }

    const bool hasFullRange = sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0;
    item.range = hasFullRange ? common::ToLspRange(sym.fullRange)
                              : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                           lsp::Position{sym.endLine, sym.endCharacter}};

    const bool hasSelection = sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0;
    item.selectionRange = hasSelection ? common::ToLspRange(sym.selectionRange)
                                       : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                                    lsp::Position{sym.endLine, sym.endCharacter}};
    return item;
}

/**
 * @brief The type declarations a bare name resolves to, if any.
 */
static std::vector<analysis::Symbol> FindTypeDeclarations(std::string_view name, const analysis::SymbolTable& table)
{
    std::vector<analysis::Symbol> found;
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

/**
 * @brief Computes the effective range of a symbol, preferring selectionRange, then fullRange, then start/end.
 * @param[in] sym Target symbol.
 * @return Effective LSP Range.
 */
static lsp::Range GetSymbolEffectiveRange(const analysis::Symbol& sym)
{
    if (sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0)
    {
        return common::ToLspRange(sym.selectionRange);
    }
    if (sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0)
    {
        return common::ToLspRange(sym.fullRange);
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
static bool MatchesItemLocation(const analysis::Symbol& sym, const lsp::TypeHierarchyItem& item,
                                const std::string& itemUri)
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
static std::vector<analysis::Symbol> ResolveItemDeclarations(const lsp::TypeHierarchyItem& item,
                                                             const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> declarations;
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
                               [&](const analysis::Symbol& sym) { return MatchesItemLocation(sym, item, itemUri); });
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
static std::vector<analysis::Symbol> ResolveBaseSymbols(const std::string& cleanBase, std::string_view declPrefix,
                                                        const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> baseSymbols;
    if (utils::ContainsScopeSeparator(cleanBase))
    {
        baseSymbols = FindTypeDeclarations(cleanBase, symbolTable);
    }
    if (baseSymbols.empty() && !declPrefix.empty())
    {
        baseSymbols = FindTypeDeclarations(utils::JoinScopedName(declPrefix, cleanBase), symbolTable);
    }
    const std::string_view baseName = analysis::LastScopeSegment(cleanBase);
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
static void CollectSupertypesForDeclaration(const analysis::Symbol& declaration,
                                            const analysis::SymbolTable& symbolTable, std::vector<std::string>& seen,
                                            std::vector<lsp::TypeHierarchyItem>& items)
{
    const std::string& declPrefix = declaration.containerName;

    for (const auto& base : analysis::GetDeclaredBases(declaration))
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
 * @brief Queries rule index for derived subtypes of target and qualifiedTarget.
 * @param[in] ruleIndex Rule index.
 * @param[in] ctx Subtype query context.
 * @param[in,out] items Output list of subtype items.
 */
static void CollectSubtypesFromRuleIndex(const analysis::rules::RuleIndex& ruleIndex, const SubtypeQueryContext& ctx,
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
static void CollectSubtypesByScan(const SubtypeQueryContext& ctx, std::vector<lsp::TypeHierarchyItem>& items)
{
    ctx.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (!IsTypeSymbol(sym) || analysis::LastScopeSegment(sym.name) == ctx.target)
                {
                    continue;
                }

                for (const auto& base : analysis::GetDeclaredBases(sym))
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

static std::string FindEnclosingNamespace(std::string_view uri, uint32_t line, const analysis::SymbolTable& table)
{
    std::string enclosingNs;
    uint32_t bestSpan = UINT32_MAX;
    const std::string fileUri(uri);
    table.ForEachSymbolInFile(fileUri,
                              [&]([[maybe_unused]] const std::string& key, const std::vector<analysis::Symbol>& symbols)
                              {
                                  for (const auto& sym : symbols)
                                  {
                                      if (sym.type != analysis::SymbolType::Namespace ||
                                          (sym.fileUri != uri && utils::PathToUri(sym.fileUri) != uri))
                                      {
                                          continue;
                                      }
                                      const auto range =
                                          sym.fullRange.endLine != 0 ? sym.fullRange : sym.selectionRange;
                                      if (line >= range.startLine && line <= range.endLine)
                                      {
                                          const uint32_t span = range.endLine - range.startLine;
                                          if (span <= bestSpan)
                                          {
                                              bestSpan = span;
                                              enclosingNs = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                                          }
                                      }
                                  }
                              });
    return enclosingNs;
}

static void FilterDeclarationsByLocation(std::string_view uri, const lsp::Position& pos,
                                         std::vector<analysis::Symbol>& declarations)
{
    if (declarations.size() <= 1)
    {
        return;
    }
    for (const auto& sym : declarations)
    {
        if ((sym.fileUri == uri || utils::PathToUri(sym.fileUri) == uri) &&
            (sym.startLine == pos.line || (sym.startLine <= pos.line && sym.endLine >= pos.line)))
        {
            declarations = {sym};
            return;
        }
    }
}

std::optional<std::vector<lsp::TypeHierarchyItem>> PrepareTypeHierarchy(const TypeHierarchyPrepareRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    const std::string name = common::FindScopedIdentifierAtPosition(request.sourceCode, request.position);

    std::vector<analysis::Symbol> declarations;
    if (!name.empty())
    {
        declarations = FindTypeDeclarations(name, request.symbolTable);
        if (declarations.empty())
        {
            const std::string enclosingNs =
                FindEnclosingNamespace(request.uri, request.position.line, request.symbolTable);
            if (!enclosingNs.empty())
            {
                declarations = FindTypeDeclarations(utils::JoinScopedName(enclosingNs, name), request.symbolTable);
            }
        }
        if (declarations.empty())
        {
            for (const auto& sym : request.symbolTable.FindTypeSymbolsByShortName(analysis::LastScopeSegment(name)))
            {
                if (IsTypeSymbol(sym))
                {
                    declarations.push_back(sym);
                }
            }
        }
        FilterDeclarationsByLocation(request.uri, request.position, declarations);
    }

    if (declarations.empty())
    {
        const std::string enclosing = request.symbolTable.FindEnclosingClassName(request.uri, request.position.line);
        if (!enclosing.empty())
        {
            declarations = FindTypeDeclarations(enclosing, request.symbolTable);
            if (declarations.empty())
            {
                for (const auto& sym :
                     request.symbolTable.FindTypeSymbolsByShortName(analysis::LastScopeSegment(enclosing)))
                {
                    if (IsTypeSymbol(sym))
                    {
                        declarations.push_back(sym);
                    }
                }
            }
        }
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
    const std::string target{analysis::LastScopeSegment(request.item.name)};
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
