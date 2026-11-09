#include "analysis/checkers/null_safety/NullSafetyCondition.h"

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace angel_lsp::analysis
{
TSNode UnwrapNullExpression(TSNode expr, const NodeIndex& index)
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(expr))
    {
        const TSSymbol sym = ts_node_symbol(expr);
        if (sym == gs.symParenthesizedExpression)
        {
            const TSNode child = SyntaxOperands(expr, &index).OnlyNamed();
            if (!ts_node_is_null(child))
            {
                expr = child;
                continue;
            }
        }
        else if (sym == gs.symUnaryExpression)
        {
            const SyntaxOperands syntax(expr, &index);
            TSNode op = syntax.Field(gs.fieldOperator);
            TSNode operand = syntax.Field(gs.fieldOperand);
            if (!ts_node_is_null(op) && !ts_node_is_null(operand) && ts_node_end_byte(op) - ts_node_start_byte(op) == 1)
            {
                expr = operand;
                continue;
            }
        }
        else if (sym == gs.symAssignmentExpression)
        {
            TSNode left = SyntaxOperands(expr, &index).Field(gs.fieldLeft);
            if (!ts_node_is_null(left))
            {
                expr = left;
                continue;
            }
        }
        break;
    }
    return expr;
}

static std::string ReadUnwrappedIdentifier(TSNode unwrapped, std::string_view sourceCode)
{
    if (ts_node_is_null(unwrapped))
    {
        return "";
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(unwrapped);
    if (sym == gs.symIdentifier)
    {
        return std::string(parser::NodeText(unwrapped, sourceCode));
    }
    if (sym == gs.symScopedIdentifier)
    {
        std::string text(parser::NodeText(unwrapped, sourceCode));
        if (!utils::ContainsScopeSeparator(text))
        {
            return text;
        }
    }
    return "";
}

std::string GetIdentifierName(TSNode node, const NullCheckContext& ctx)
{
    return ReadUnwrappedIdentifier(UnwrapNullExpression(node, ctx.index), ctx.sourceCode);
}

static std::string ExtractNullComparedIdentifier(const BinaryExpressionOperands& operands, const NullCheckContext& ctx)
{
    if (IsNullInitializer(operands.right, &ctx.index))
    {
        return GetIdentifierName(operands.left, ctx);
    }
    if (IsNullInitializer(operands.left, &ctx.index))
    {
        return GetIdentifierName(operands.right, ctx);
    }
    return {};
}

static NullConditionAssertions AssertionsFor(std::string name, bool isEquality)
{
    if (name.empty())
    {
        return {};
    }
    return {{{name, isEquality ? Nullability::DefinitelyNull : Nullability::NonNull}},
            {{std::move(name), isEquality ? Nullability::NonNull : Nullability::DefinitelyNull}}};
}

static void AppendAssertions(std::vector<NullAssertion>& target, std::vector<NullAssertion>& source)
{
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

static void KeepCommonAssertions(std::vector<NullAssertion>& left, const std::vector<NullAssertion>& right)
{
    std::erase_if(left,
                  [&](const NullAssertion& item)
                  {
                      return std::ranges::none_of(
                          right, [&](const NullAssertion& other)
                          { return item.varName == other.varName && item.state == other.state; });
                  });
}

const NullConditionAssertions& NullConditionWorklist::Remember(TSNode condition)
{
    const TSNode node = UnwrapNullExpression(condition, m_ctx.index);
    if (ts_node_is_null(node))
    {
        static const NullConditionAssertions empty;
        return empty;
    }
    const NodeKey key{node.id, ts_node_start_byte(node)};
    auto assertions = Take(node);
    return m_summaries.try_emplace(key, std::move(assertions)).first->second;
}

NullConditionAssertions NullConditionWorklist::Take(TSNode condition)
{
    m_pending.push_back({NullConditionAction::Evaluate, condition});
    while (!m_pending.empty())
    {
        const NullConditionWork work = m_pending.back();
        m_pending.pop_back();
        if (work.action == NullConditionAction::Evaluate)
        {
            Evaluate(work.node);
        }
        else if (work.action == NullConditionAction::Negate)
        {
            auto& value = m_values.back();
            value.positive.swap(value.negative);
        }
        else
        {
            Merge(work.action == NullConditionAction::And);
        }
    }
    auto result = std::move(m_values.back());
    m_values.pop_back();
    return result;
}

void NullConditionWorklist::Merge(bool isAnd)
{
    auto right = std::move(m_values.back());
    m_values.pop_back();
    auto& left = m_values.back();
    if (isAnd)
    {
        AppendAssertions(left.positive, right.positive);
    }
    else
    {
        KeepCommonAssertions(left.positive, right.positive);
    }
    AppendAssertions(left.negative, right.negative);
}

void NullConditionWorklist::EvaluateBinary(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto operands = ReadBinaryOperands(node, &m_ctx.index);
    if (!operands.Complete())
    {
        m_values.emplace_back();
        return;
    }
    const auto op = ts_node_symbol(operands.op);
    if (op == gs.tokIs || op == gs.tokEqEq || op == gs.tokBangIs || op == gs.tokBangEq)
    {
        m_values.push_back(
            AssertionsFor(ExtractNullComparedIdentifier(operands, m_ctx), op == gs.tokIs || op == gs.tokEqEq));
        return;
    }
    const bool isAnd = op == gs.tokAmpAmp || op == gs.tokAnd;
    if (isAnd || op == gs.tokPipePipe || op == gs.tokOr)
    {
        m_pending.push_back({isAnd ? NullConditionAction::And : NullConditionAction::Or});
        m_pending.push_back({NullConditionAction::Evaluate, operands.right});
        m_pending.push_back({NullConditionAction::Evaluate, operands.left});
        return;
    }
    m_values.emplace_back();
}

void NullConditionWorklist::Evaluate(TSNode condition)
{
    const TSNode node = UnwrapNullExpression(condition, m_ctx.index);
    if (ts_node_is_null(node))
    {
        m_values.emplace_back();
        return;
    }
    if (const auto it = m_summaries.find(NodeKey{node.id, ts_node_start_byte(node)}); it != m_summaries.end())
    {
        m_values.push_back(std::move(it->second));
        m_summaries.erase(it);
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const auto symbol = ts_node_symbol(node);
    if (symbol == gs.symBinaryExpression)
    {
        EvaluateBinary(node);
    }
    else if (symbol == gs.symUnaryExpression)
    {
        const SyntaxOperands syntax(node, &m_ctx.index);
        if (syntax.HasUnaryOperator(gs.tokBang) || syntax.HasUnaryOperator(gs.tokNot))
        {
            m_pending.push_back({NullConditionAction::Negate});
            m_pending.push_back({NullConditionAction::Evaluate, syntax.Field(gs.fieldOperand)});
        }
        else
        {
            m_values.emplace_back();
        }
    }
    else
    {
        m_values.push_back(AssertionsFor(ReadUnwrappedIdentifier(node, m_ctx.sourceCode), false));
    }
}

} // namespace angel_lsp::analysis
