#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <ankerl/unordered_dense.h>
#include <charconv>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>

namespace angel_lsp::analysis
{

/**
 * @brief True for a case value shape AngelScript cannot use as a label.
 */
static bool IsUnusableCaseValue(TSNode expression, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(expression);

    if (sym == gs.symStringLiteral)
    {
        return true;
    }

    if (sym == gs.symNumberLiteral)
    {
        // A case label has to be an integral constant, so a fractional literal is out.
        // Identifiers are left alone: an enum member and a `const int` both arrive as one
        // and both are legal.
        const std::string_view text = parser::NodeText(expression, sourceCode);
        return text.find('.') != std::string_view::npos;
    }

    return false;
}

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
            // No value written means the compiler counts it up from the previous one.
            // Following that count is possible and is not done here: the value then
            // depends on every member before it, and a wrong number would mean claiming
            // a duplicate that is not one.
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

/**
 * @brief The number a case label stands for, when that can be known for certain.
 *        `enum E { A = 1, B = 1 }` gives two names to one number, and a switch dispatches on the
 * @brief Resolve an enumerator's integer value from the cached RuleIndex.
 *        Switch-case duplicate detection needs this so that two cases written with different
 */
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

        // Written with a qualifier: only the enum it names may answer.
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

    // Written without a qualifier and declared by more than one enum: which one this label
    // means is the compiler's business and not knowable from here.
    if (declaringEnums != 1)
    {
        return std::nullopt;
    }

    return found;
}

static bool CheckLocalCaseConstantness(TSNode value, std::string_view idText, DiagnosticContext& ctx)
{
    if (!ctx.request.scopeRoot)
    {
        return false;
    }

    const TSPoint at = ts_node_start_point(value);
    const Scope* scope = FindInnermostScope(ctx.request.scopeRoot.get(), at.row, at.column);
    if (!scope)
    {
        return false;
    }

    const Scope* owner = nullptr;
    const LocalDefinition* def = ResolveInScopeAt(scope, idText, ts_node_start_byte(value), &owner);

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
        ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-case-not-constant");
        return true;
    }

    return false;
}

static void CheckCaseConstantness(TSNode value, std::string_view sourceCode, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(value);
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        const std::string_view idText = utils::TrimView(parser::NodeText(value, sourceCode));

        bool reported = false;
        auto syms =
            FindSymbolsInScope(std::string(idText), value, sourceCode, ctx.request.symbolTable, ctx.request.nodeIndex);
        for (const auto& s : syms)
        {
            if (s.type == SymbolType::Variable && !s.GetVariable().modifiers.isConst)
            {
                ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-case-not-constant");
                reported = true;
                break;
            }
        }

        if (!reported)
        {
            CheckLocalCaseConstantness(value, idText, ctx);
        }
    }
    else if (sym == gs.symCallExpression)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-case-not-constant");
    }
}

static std::string ComputeCaseKey(std::string_view text, const rules::RuleIndex& ruleIndex)
{
    if (const auto resolved = EnumeratorValue(text, ruleIndex))
    {
        return "#" + std::to_string(*resolved);
    }
    if (const auto parsed = ParseDecimalInteger(text))
    {
        return "#" + std::to_string(*parsed);
    }
    return std::string(text);
}

void CheckSwitchLabels(std::span<const TSNode> labels, std::string_view sourceCode, DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string> seenValues;
    for (const TSNode value : labels)
    {
        if (IsUnusableCaseValue(value, sourceCode))
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-invalid-case-type");
        }

        CheckCaseConstantness(value, sourceCode, ctx);
        const auto text = utils::TrimView(parser::NodeText(value, sourceCode));
        if (!text.empty() && !seenValues.insert(ComputeCaseKey(text, ctx.request.GetRuleIndex())).second)
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(value), "as-err-duplicate-case-value", text);
        }
    }
}

} // namespace angel_lsp::analysis
