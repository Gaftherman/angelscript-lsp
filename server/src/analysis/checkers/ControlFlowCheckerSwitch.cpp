#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ControlFlowFacts.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "utils/ScopedNameUtils.h"

#include <ankerl/unordered_dense.h>
#include <charconv>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

static std::optional<long long> ParseDecimalInteger(std::string_view text)
{
    if (text.starts_with('+'))
    {
        text.remove_prefix(1);
        if (text.starts_with('-'))
        {
            return std::nullopt;
        }
    }
    if (text.empty())
    {
        return std::nullopt;
    }

    long long result = 0;
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
    {
        return std::nullopt;
    }
    return result;
}

static std::optional<long long> ExtractMemberValue(const EnumSignature& enumSig, std::string_view label, bool& matched)
{
    for (const auto& member : enumSig.members)
    {
        if (member.name == label)
        {
            matched = true;
            if (!member.value.empty())
            {
                return ParseDecimalInteger(member.value);
            }
            break;
        }
    }
    return std::nullopt;
}

static std::pair<std::string_view, std::string_view> SplitEnumeratorQualifier(std::string_view label)
{
    const auto [qualifier, leaf] = utils::SplitScopedName(label);
    const std::string_view enumName = utils::LastScopeSegment(qualifier);
    return {enumName, leaf};
}

static std::optional<long long> EnumeratorValue(std::string_view label, const rules::RuleIndex& ruleIndex)
{
    auto [enumName, memberLabel] = SplitEnumeratorQualifier(label);
    if (memberLabel.empty())
    {
        return std::nullopt;
    }

    const auto it = ruleIndex.enumSymbolsByMemberName.find(memberLabel);
    if (it == ruleIndex.enumSymbolsByMemberName.end())
    {
        return std::nullopt;
    }

    std::optional<long long> found;
    size_t declaringEnums = 0;

    for (const auto& sym : it->second)
    {
        if (sym.type != SymbolType::Enum || !std::holds_alternative<EnumSignature>(sym.signature))
        {
            continue;
        }

        if (!enumName.empty() && sym.name != enumName)
        {
            continue;
        }

        bool matched = false;
        auto val = ExtractMemberValue(sym.GetEnum(), memberLabel, matched);
        if (matched)
        {
            ++declaringEnums;
            if (val)
            {
                found = val;
            }
        }
    }

    if (declaringEnums != 1)
    {
        return std::nullopt;
    }

    return found;
}

static bool CheckLocalCaseConstantness(const ControlFlowCaseLabelFact& label, std::string_view idText,
                                       DiagnosticContext& ctx)
{
    if (!ctx.request.scopeRoot)
    {
        return false;
    }

    const Scope* scope =
        FindInnermostScope(ctx.request.scopeRoot.get(), label.range.startLine, label.range.startCharacter);
    if (!scope)
    {
        return false;
    }

    const Scope* owner = nullptr;
    const LocalDefinition* def = ResolveInScopeAt(scope, idText, label.bytes.startByte, &owner);

    bool insideFunction = false;
    for (const Scope* current = owner; current != nullptr; current = current->parent)
    {
        if (current->isFunctionScope)
        {
            insideFunction = true;
            break;
        }
    }

    if (def && insideFunction && def->kind == LocalDefinitionKind::Variable)
    {
        ctx.EmitAtRange(label.range, "as-err-case-not-constant");
        return true;
    }

    return false;
}

static void CheckCaseConstantness(const ControlFlowCaseLabelFact& label, std::string_view sourceCode,
                                  DiagnosticContext& ctx)
{
    if (label.isIdentifier)
    {
        const std::string_view idText = label.text;

        bool reported = false;
        std::vector<ContainerInfo> containers;
        if (ctx.request.nodeIndex)
        {
            containers = ctx.request.nodeIndex->CollectLookupContainers(label.bytes.startByte, label.bytes.endByte, 0,
                                                                        sourceCode);
        }
        const auto& usings = GetUsingNamespaces(ctx.request);
        const auto syms = FindSymbolsInScope(ctx.request.symbolTable, containers, idText, usings);
        for (const auto& s : syms)
        {
            if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
            {
                ctx.EmitAtRange(label.range, "as-err-case-not-constant");
                reported = true;
                break;
            }
        }

        if (!reported)
        {
            CheckLocalCaseConstantness(label, idText, ctx);
        }
    }
    else if (label.isCall)
    {
        ctx.EmitAtRange(label.range, "as-err-case-not-constant");
    }
}

/**
 * @brief Records a case label and reports whether an equal label was already seen.
 * @param[in] text Label spelling, viewing the analysed document.
 * @param[in] ruleIndex Rule index that resolves enumerator labels to their values.
 * @param[in,out] seenNumbers Numeric values of earlier labels; an enumerator and a literal of equal value collide.
 * @param[in,out] seenTexts Spellings of earlier labels that have no numeric value.
 * @return True when the label was new.
 */
static bool InsertCaseLabel(std::string_view text, const rules::RuleIndex& ruleIndex,
                            ankerl::unordered_dense::set<long long>& seenNumbers,
                            ankerl::unordered_dense::set<std::string_view>& seenTexts)
{
    if (const auto resolved = EnumeratorValue(text, ruleIndex))
    {
        return seenNumbers.insert(*resolved).second;
    }
    if (const auto parsed = ParseDecimalInteger(text))
    {
        return seenNumbers.insert(*parsed).second;
    }
    return seenTexts.insert(text).second;
}

void CheckSwitchLabels(std::span<const ControlFlowCaseLabelFact> labels, std::string_view sourceCode,
                       DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<long long> seenNumbers;
    ankerl::unordered_dense::set<std::string_view> seenTexts;
    for (const auto& label : labels)
    {
        if (label.isUnusable)
        {
            ctx.EmitAtRange(label.range, "as-err-invalid-case-type");
        }

        CheckCaseConstantness(label, sourceCode, ctx);
        const auto text = label.text;
        if (!text.empty() && !InsertCaseLabel(text, ctx.request.GetRuleIndex(), seenNumbers, seenTexts))
        {
            ctx.EmitAtRange(label.range, "as-err-duplicate-case-value", text);
        }
    }
}

void CheckSwitchLabels(const ControlFlowRecord& record, const ControlFlowFacts& facts, std::string_view sourceCode,
                       DiagnosticContext& ctx)
{
    std::vector<IndexedByteRange> innerSwitches;
    for (const auto& r : facts.records)
    {
        if (r.kind == ControlFlowRecordKind::Switch && record.bytes.startByte < r.bytes.startByte &&
            r.bytes.endByte <= record.bytes.endByte)
        {
            innerSwitches.push_back(r.bytes);
        }
    }

    std::vector<ControlFlowCaseLabelFact> switchLabels;
    for (const auto& label : facts.labels)
    {
        if (label.bytes.startByte >= record.bytes.startByte && label.bytes.endByte <= record.bytes.endByte)
        {
            bool inInner = false;
            for (const auto& inner : innerSwitches)
            {
                if (label.bytes.startByte >= inner.startByte && label.bytes.endByte <= inner.endByte)
                {
                    inInner = true;
                    break;
                }
            }
            if (!inInner)
            {
                switchLabels.push_back(label);
            }
        }
    }

    CheckSwitchLabels(switchLabels, sourceCode, ctx);
}

} // namespace angel_lsp::analysis
