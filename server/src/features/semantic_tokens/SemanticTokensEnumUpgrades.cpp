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

ankerl::unordered_dense::map<uint32_t, uint32_t>
PrecalculateScopedEnumUpgrades(const analysis::SemanticTokenFacts& facts, std::string_view sourceCode,
                               const analysis::SymbolTable& symbolTable)
{
    ankerl::unordered_dense::map<uint32_t, uint32_t> upgrades;
    for (const auto& scopedName : facts.scopedNames)
    {
        UpgradeScopedIdentifierEnumTokens(scopedName, sourceCode, symbolTable, upgrades);
    }
    for (const auto& scopedType : facts.scopedTypes)
    {
        UpgradeScopedTypeEnumTokens(scopedType, sourceCode, symbolTable, upgrades);
    }
    return upgrades;
}

} // namespace angel_lsp::features
