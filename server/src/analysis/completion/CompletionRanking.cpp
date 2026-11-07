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

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief Formats sort text prefix for a given type match rank.
 * @param[in] rank Computed type match rank.
 * @param[in] baseSort Base sort key or label.
 * @return Formatted sortText string with bucket prefix.
 */
static inline std::string FormatRankedSortText(TypeMatchRank rank, std::string_view baseSort)
{
    static constexpr std::array<std::string_view, 3> k_rankPrefixes = {"0000_", "0001_", "0002_"};
    const size_t idx = static_cast<size_t>(rank);
    if (idx < k_rankPrefixes.size())
    {
        return std::string(k_rankPrefixes[idx]) + std::string(baseSort);
    }
    return std::string("9999_") + std::string(baseSort);
}

/**
 * @brief Computes type ranking score (Exact match, Convertible, or Other).
 * @param[in] item Completion item to rank.
 * @param[in] expectedType Target expected type.
 * @param[in] symbolTable Global symbol table.
 * @return TypeMatchRank tier.
 */
static TypeMatchRank ComputeTypeRank(const CompletionEntry& item, const std::string& expectedType,
                                     const analysis::SymbolTable& symbolTable)
{
    std::string cleanExpected = analysis::StripTypeDecorations(expectedType);
    std::string itemType = item.detail.has_value() ? *item.detail : "";
    std::string cleanItem = analysis::StripTypeDecorations(itemType);

    if (!cleanExpected.empty())
    {
        if (item.kind.has_value() && *item.kind == CompletionEntryKind::Class && item.label == cleanExpected)
        {
            return TypeMatchRank::Exact;
        }
        if (!cleanItem.empty() && cleanItem == cleanExpected)
        {
            return TypeMatchRank::Exact;
        }
    }

    if (parser::primitives::IsNumeric(cleanExpected) && parser::primitives::IsNumeric(cleanItem))
    {
        return TypeMatchRank::Convertible;
    }
    if (!cleanExpected.empty() && !cleanItem.empty())
    {
        auto hier = GetInheritedTypeHierarchy(symbolTable, cleanItem);
        for (const auto& base : hier)
        {
            if (base == cleanExpected)
            {
                return TypeMatchRank::Convertible;
            }
        }
    }

    return TypeMatchRank::Other;
}

void ApplySmartTypeRanking(std::vector<CompletionEntry>& items, std::string_view prefix, const analysis::Scope* scope,
                           const CompletionQuery& request)
{
    std::string callee;
    size_t argIndex = 0;
    std::string expectedType;
    if (ExtractCallContext(prefix, callee, argIndex))
    {
        expectedType = FindCalleeParameterType(callee, argIndex, request, scope);
    }
    if (expectedType.empty())
    {
        expectedType = ExtractAssignmentTargetType(prefix);
    }
    if (expectedType.empty())
    {
        return;
    }

    for (auto& item : items)
    {
        const TypeMatchRank rank = ComputeTypeRank(item, expectedType, request.symbolTable);
        const std::string_view baseSort = item.sortText.has_value() ? *item.sortText : item.label;
        item.sortText = FormatRankedSortText(rank, baseSort);
    }
}

void SortCompletionItemsByProximity(std::vector<CompletionEntry>& items)
{
    std::stable_sort(items.begin(), items.end(),
                     [](const CompletionEntry& a, const CompletionEntry& b)
                     {
                         const std::string& sa = a.sortText.has_value() ? *a.sortText : a.label;
                         const std::string& sb = b.sortText.has_value() ? *b.sortText : b.label;
                         return sa < sb;
                     });
}

} // namespace angel_lsp::analysis::completion
