#include "features/semantic_tokens/SemanticTokensInternal.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Checks if a symbol list contains any enum constant.
 * @param[in] symbols Symbol vector handle, or nullptr.
 * @return True if at least one symbol is an enum member.
 */
static bool ContainsEnumConstant(const std::shared_ptr<const std::vector<analysis::Symbol>>& symbols)
{
    if (!symbols || symbols->empty())
    {
        return false;
    }
    return std::any_of(symbols->begin(), symbols->end(), [](const analysis::Symbol& s)
                       { return s.type == analysis::SymbolType::Variable && s.GetVariable().isEnumConstant; });
}

/**
 * @brief Checks if all symbols in a vector are enum declarations.
 * @param[in] symbols Symbol vector handle, or nullptr.
 * @return True if symbols non-empty and all represent enums.
 */
static bool AreAllEnumTypes(const std::shared_ptr<const std::vector<analysis::Symbol>>& symbols)
{
    if (!symbols || symbols->empty())
    {
        return false;
    }
    return std::all_of(symbols->begin(), symbols->end(),
                       [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Enum; });
}

/**
 * @brief Applies RuleIndex-based enum qualifier and member upgrades for a scoped_identifier.
 * @param[in] item Pre-extracted scoped identifier fact.
 * @param[in] sourceCode Source text buffer.
 * @param[in] ruleIndex Pre-indexed workspace symbol metadata.
 * @param[in,out] upgrades Map from start byte to upgraded token type.
 */
static void ApplyRuleIndexEnumUpgrades(const analysis::IndexedScopedName& item, std::string_view sourceCode,
                                       const analysis::rules::RuleIndex& ruleIndex,
                                       ankerl::unordered_dense::map<uint32_t, uint32_t>& upgrades)
{
    const std::string_view leftText = utils::SliceSource(sourceCode, item.leftStartByte, item.leftEndByte);
    const std::string_view rightText = utils::SliceSource(sourceCode, item.rightStartByte, item.rightEndByte);
    const std::string_view prevText = utils::SliceSource(sourceCode, item.prevStartByte, item.prevEndByte);
    auto enumIt = ruleIndex.enumSymbolsByMemberName.find(rightText);
    if (enumIt == ruleIndex.enumSymbolsByMemberName.end())
    {
        return;
    }
    const uint32_t rightStart = item.rightStartByte;
    for (const auto& enumSym : enumIt->second)
    {
        if (prevText == enumSym.name)
        {
            upgrades[item.prevStartByte] = analysis::Type_Enum;
            upgrades[rightStart] = analysis::Type_EnumMember;
            return;
        }
        if (leftText == enumSym.containerName || prevText == enumSym.containerName ||
            prevText == utils::LastScopeSegment(enumSym.containerName))
        {
            upgrades[rightStart] = analysis::Type_EnumMember;
        }
    }
}

/**
 * @brief Upgrades a single scoped_identifier node's enum and enum-member segments.
 * @param[in] item Pre-extracted scoped identifier fact.
 * @param[in] sourceCode Source text buffer.
 * @param[in] symbolTable Global symbol table.
 * @param[in,out] upgrades Map from start byte to upgraded token type.
 */
static void UpgradeScopedIdentifierEnumTokens(const analysis::IndexedScopedName& item, std::string_view sourceCode,
                                              const analysis::SymbolTable& symbolTable,
                                              ankerl::unordered_dense::map<uint32_t, uint32_t>& upgrades)
{
    const std::string_view leftText = utils::SliceSource(sourceCode, item.leftStartByte, item.leftEndByte);
    const std::string_view rightText = utils::SliceSource(sourceCode, item.rightStartByte, item.rightEndByte);
    if (leftText.empty() || rightText.empty())
    {
        return;
    }

    const uint32_t leftStart = item.leftStartByte;
    const uint32_t rightStart = item.rightStartByte;
    if (AreAllEnumTypes(symbolTable.FindSymbolsPtr(leftText)))
    {
        upgrades[leftStart] = analysis::Type_Enum;
        upgrades[rightStart] = analysis::Type_EnumMember;
        return;
    }
    const std::string_view scopedText = utils::SliceSource(sourceCode, item.nodeStartByte, item.nodeEndByte);
    if (ContainsEnumConstant(symbolTable.FindSymbolsPtr(scopedText)))
    {
        upgrades[rightStart] = analysis::Type_EnumMember;
    }
    if (const auto ruleIndex = symbolTable.GetRuleIndex())
    {
        ApplyRuleIndexEnumUpgrades(item, sourceCode, *ruleIndex, upgrades);
    }
}

/**
 * @brief Upgrades scoped enum type qualifiers and member tokens in type nodes.
 * @param[in] item Pre-extracted scoped type fact.
 * @param[in] sourceCode Source text buffer.
 * @param[in] symbolTable Global symbol table.
 * @param[in,out] upgrades Map from start byte to upgraded token type.
 */
static void UpgradeScopedTypeEnumTokens(const analysis::IndexedScopedType& item, std::string_view sourceCode,
                                        const analysis::SymbolTable& symbolTable,
                                        ankerl::unordered_dense::map<uint32_t, uint32_t>& upgrades)
{
    const std::string_view scopeText = utils::SliceSource(sourceCode, item.scopeTextStartByte, item.scopeTextEndByte);
    const std::string_view memberText =
        utils::SliceSource(sourceCode, item.memberTextStartByte, item.memberTextEndByte);
    if (scopeText.empty() || memberText.empty())
    {
        return;
    }

    if (AreAllEnumTypes(symbolTable.FindSymbolsPtr(scopeText)))
    {
        upgrades[item.scopeUpgradeByte] = analysis::Type_Enum;
        upgrades[item.memberUpgradeByte] = analysis::Type_EnumMember;
        return;
    }
    if (ContainsEnumConstant(symbolTable.FindMemberSymbolPtr(scopeText, memberText)))
    {
        upgrades[item.memberUpgradeByte] = analysis::Type_EnumMember;
    }
}

/**
 * @brief Computes half-open byte bounds in sourceCode for an LSP range.
 * @param[in] sourceCode Source text buffer.
 * @param[in] range Target LSP range.
 * @return Pair of [startByte, endByte).
 */
static std::pair<uint32_t, uint32_t> ComputeRangeBytes(std::string_view sourceCode, const lsp::Range& range) noexcept
{
    uint32_t startByte = static_cast<uint32_t>(sourceCode.size());
    uint32_t endByte = static_cast<uint32_t>(sourceCode.size());
    uint32_t line = 0;
    size_t pos = 0;
    bool foundStart = false;

    while (pos <= sourceCode.size())
    {
        const size_t nl = sourceCode.find('\n', pos);
        const size_t lineEnd = (nl == std::string_view::npos) ? sourceCode.size() : nl;
        if (!foundStart && line == range.start.line)
        {
            startByte = static_cast<uint32_t>(std::min(pos + range.start.character, lineEnd));
            foundStart = true;
        }
        if (line == range.end.line)
        {
            endByte = static_cast<uint32_t>(std::min(pos + range.end.character, lineEnd));
            break;
        }
        if (nl == std::string_view::npos)
        {
            break;
        }
        pos = nl + 1;
        ++line;
    }

    if (endByte < startByte)
    {
        endByte = startByte;
    }
    return {startByte, endByte};
}

ankerl::unordered_dense::map<uint32_t, uint32_t>
PrecalculateScopedEnumUpgrades(const analysis::SemanticTokenFacts& facts, std::string_view sourceCode,
                               const analysis::SymbolTable& symbolTable, const std::optional<lsp::Range>& range)
{
    ankerl::unordered_dense::map<uint32_t, uint32_t> upgrades;
    if (facts.scopedNames.empty() && facts.scopedTypes.empty())
    {
        return upgrades;
    }

    const std::optional<std::pair<uint32_t, uint32_t>> byteBounds =
        range.has_value() ? std::optional<std::pair<uint32_t, uint32_t>>(ComputeRangeBytes(sourceCode, *range))
                          : std::nullopt;

    for (const auto& scopedName : facts.scopedNames)
    {
        if (byteBounds.has_value() &&
            (scopedName.nodeEndByte <= byteBounds->first || scopedName.nodeStartByte >= byteBounds->second))
        {
            continue;
        }
        UpgradeScopedIdentifierEnumTokens(scopedName, sourceCode, symbolTable, upgrades);
    }
    for (const auto& scopedType : facts.scopedTypes)
    {
        if (byteBounds.has_value() &&
            (scopedType.memberTextEndByte <= byteBounds->first || scopedType.scopeTextStartByte >= byteBounds->second))
        {
            continue;
        }
        UpgradeScopedTypeEnumTokens(scopedType, sourceCode, symbolTable, upgrades);
    }
    return upgrades;
}

} // namespace angel_lsp::features
