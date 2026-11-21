#include "features/type_hierarchy/TypeHierarchyHandler.h"

#include "analysis/index/NamespaceUsageFacts.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/Constants.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <optional>
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
    item.name.assign(analysis::LastScopeSegment(sym.name));
    item.kind = sym.type == analysis::SymbolType::Interface ? lsp::SymbolKind::Interface : lsp::SymbolKind::Class;
    item.uri = lsp::DocumentUri::parse(sym.fileUri);
    item.data = lsp::json::Value(lsp::String(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName));
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
 * @brief Looks up class and interface declarations for an exact symbol name.
 * @param[in] name Name to look up, optionally qualified relative to scope.
 * @param[in] table Symbol table containing the declarations.
 * @param[in] scope Enclosing namespace, or empty to look up name directly.
 * @return Matching type declarations, or an empty vector if none exist.
 */
static std::vector<analysis::Symbol> FindTypeDeclarations(std::string_view name, const analysis::SymbolTable& table,
                                                          std::string_view scope = {})
{
    std::vector<analysis::Symbol> found;
    const auto symbols = scope.empty() ? table.FindSymbolsPtr(name) : table.FindMemberSymbolPtr(scope, name);
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
                                std::string_view itemUri)
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
        const auto itemUri = item.uri.toString();
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
static std::vector<analysis::Symbol> ResolveBaseSymbols(std::string_view cleanBase, std::string_view declPrefix,
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
 * @brief Collects supertype items for a single declaration, skipping types already emitted.
 * @param[in] declaration Declaring type symbol.
 * @param[in] symbolTable Symbol table.
 * @param[in,out] items Output list of supertype items; each item's data payload holds its qualified key, which is
 *        what deduplication compares against.
 */
static void CollectSupertypesForDeclaration(const analysis::Symbol& declaration,
                                            const analysis::SymbolTable& symbolTable,
                                            std::vector<lsp::TypeHierarchyItem>& items)
{
    const std::string_view declPrefix = declaration.containerName;

    for (const auto& base : analysis::GetDeclaredBases(declaration))
    {
        const std::string_view cleanBase = analysis::CleanBaseTypeView(base);
        if (cleanBase.empty())
        {
            continue;
        }

        const auto baseSymbols = ResolveBaseSymbols(cleanBase, declPrefix, symbolTable);
        for (const auto& baseSymbol : baseSymbols)
        {
            const std::string_view key = baseSymbol.qualifiedName.empty() ? baseSymbol.name : baseSymbol.qualifiedName;
            const bool alreadyEmitted = std::any_of(
                items.begin(), items.end(), [key](const lsp::TypeHierarchyItem& emitted)
                { return emitted.data.has_value() && emitted.data->isString() && emitted.data->string() == key; });
            if (!alreadyEmitted)
            {
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
    // Views into ruleIndex entries, which the caller keeps alive for the whole query.
    ankerl::unordered_dense::set<std::string_view> seen;
    auto collectFrom = [&](std::string_view baseKey)
    {
        auto it = ruleIndex.derivedByBase.find(baseKey);
        if (it == ruleIndex.derivedByBase.end())
        {
            return;
        }
        for (const auto& derived : it->second)
        {
            const std::string_view key = derived.qualifiedName.empty() ? derived.name : derived.qualifiedName;
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
        [&]([[maybe_unused]] std::string_view qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (!IsTypeSymbol(sym) || analysis::LastScopeSegment(sym.name) == ctx.target)
                {
                    continue;
                }

                for (const auto& base : analysis::GetDeclaredBases(sym))
                {
                    const std::string_view cleanBase = analysis::CleanBaseTypeView(base);
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

/** @brief Resolves an indexed owner to the declaration at its exact name span. */
static std::optional<lsp::TypeHierarchyItem> FindOwnerItem(const TypeHierarchyPrepareRequest& request,
                                                           const analysis::EnclosingType& owner)
{
    const auto candidates = request.symbolTable.FindSymbolsPtr(owner.qualifiedName);
    if (!candidates)
    {
        return std::nullopt;
    }
    const auto sameNameSpan = [&](const analysis::Symbol& sym)
    {
        const auto& lines = request.nodeIndex.Lines();
        const auto& range = sym.selectionRange;
        const auto start = lines.StartOffset(request.parsedSource, range.startLine) + range.startCharacter;
        const auto end = lines.StartOffset(request.parsedSource, range.endLine) + range.endCharacter;
        return start == owner.nameBytes.startByte && end == owner.nameBytes.endByte;
    };
    const auto match =
        std::ranges::find_if(*candidates, [&](const analysis::Symbol& sym)
                             { return IsTypeSymbol(sym) && sym.fileUri == request.uri && sameNameSpan(sym); });
    return match == candidates->end() ? std::nullopt : std::optional{ToItem(*match)};
}

/** @brief Resolves the selected segment before trying its full scoped spelling and short name. */
static std::vector<analysis::Symbol> FindReferencedTypes(const TypeHierarchyPrepareRequest& request,
                                                         uint32_t cursorByte)
{
    const auto spelling = analysis::FindScopedNameAtCursor(request.nodeIndex, request.parsedSource, cursorByte);
    if (!spelling)
    {
        return {};
    }
    const auto name = spelling->throughSelection;
    auto declarations = FindTypeDeclarations(name, request.symbolTable);
    if (declarations.empty())
    {
        const auto enclosingNamespace = request.nodeIndex.FindEnclosingNamespace(cursorByte, request.parsedSource);
        if (!enclosingNamespace.empty())
        {
            declarations = FindTypeDeclarations(name, request.symbolTable, enclosingNamespace);
        }
    }
    if (declarations.empty() && spelling->full != name)
    {
        declarations = FindTypeDeclarations(spelling->full, request.symbolTable);
    }
    if (declarations.empty())
    {
        declarations = request.symbolTable.FindTypeSymbolsByShortName(analysis::LastScopeSegment(name));
        std::erase_if(declarations, [](const analysis::Symbol& sym) { return !IsTypeSymbol(sym); });
    }
    return declarations;
}

std::optional<std::vector<lsp::TypeHierarchyItem>> PrepareTypeHierarchy(const TypeHierarchyPrepareRequest& request)
{
    const auto& lines = request.nodeIndex.Lines();
    const auto row = request.position.line;
    if (row >= lines.LineCount())
    {
        return std::nullopt;
    }

    const auto line = lines.Line(request.sourceCode, row);
    const auto lineStart = lines.StartOffset(request.sourceCode, row);
    const auto column = utils::LspCharToByteColumn(line, request.position.character, request.positionEncoding);
    const auto cursorByte = static_cast<uint32_t>(lineStart + column);
    const auto owner = request.nodeIndex.FindEnclosingType(cursorByte, request.parsedSource,
                                                           analysis::EnclosingTypeBoundary::CrossLambda);
    const bool onOwnerName = owner && owner->nameBytes.Contains(cursorByte);
    if (onOwnerName)
    {
        if (const auto item = FindOwnerItem(request, *owner))
        {
            return std::vector{*item};
        }
    }

    const auto declarations = FindReferencedTypes(request, cursorByte);
    if (declarations.empty())
    {
        if (owner && !onOwnerName)
        {
            if (const auto item = FindOwnerItem(request, *owner))
            {
                return std::vector{*item};
            }
        }
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
    for (const auto& declaration : declarations)
    {
        CollectSupertypesForDeclaration(declaration, request.symbolTable, items);
    }

    return items.empty() ? std::nullopt : std::optional{items};
}

std::optional<std::vector<lsp::TypeHierarchyItem>> GetSubtypes(const TypeHierarchyItemRequest& request)
{
    const std::string_view target = analysis::LastScopeSegment(request.item.name);
    if (target.empty())
    {
        return std::nullopt;
    }

    std::string_view qualifiedTarget;
    if (request.item.data.has_value() && request.item.data->isString())
    {
        qualifiedTarget = request.item.data->string();
    }

    const SubtypeQueryContext ctx{target, qualifiedTarget, request.item.name, request.symbolTable};
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
