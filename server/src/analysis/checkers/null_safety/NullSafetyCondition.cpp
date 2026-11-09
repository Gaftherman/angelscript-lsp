#include "analysis/checkers/null_safety/NullSafetyCondition.h"

#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Follows captured expression links to find the value used for null tracking.
 * @param[in] expr Expression node id.
 * @param[in] facts Null flow facts arena.
 * @return Unwrapped expression node id.
 */
NullFlowNodeId UnwrapNullExpression(NullFlowNodeId expr, const NullFlowFacts& facts)
{
    while (expr != NoNullFlowNode)
    {
        const NullFlowNode* node = facts.Find(expr);
        if (!node)
        {
            break;
        }
        if (node->kind == NullFlowNodeKind::Parenthesized)
        {
            if (node->soleOperand && node->first != NoNullFlowNode)
            {
                expr = node->first;
                continue;
            }
        }
        else if (node->kind == NullFlowNodeKind::Unary)
        {
            if (node->singleCharOperator && node->first != NoNullFlowNode)
            {
                expr = node->first;
                continue;
            }
        }
        else if (node->kind == NullFlowNodeKind::Assignment)
        {
            if (node->first != NoNullFlowNode)
            {
                expr = node->first;
                continue;
            }
        }
        break;
    }
    return expr;
}

static std::string_view ReadUnwrappedIdentifier(const NullFlowNode* node)
{
    if (!node)
    {
        return {};
    }
    if (node->kind == NullFlowNodeKind::Identifier)
    {
        return node->text;
    }
    if (node->kind == NullFlowNodeKind::ScopedIdentifier)
    {
        if (!utils::ContainsScopeSeparator(node->text))
        {
            return node->text;
        }
    }
    return {};
}

/**
 * @brief Extracts a simple identifier using the captured expression operands.
 * @param[in] node Node id of the expression.
 * @param[in] ctx Null check evaluation context.
 * @return Identifier name view, or empty view if none.
 */
std::string_view GetIdentifierName(NullFlowNodeId node, const NullCheckContext& ctx)
{
    const NullFlowNodeId unwrapped = UnwrapNullExpression(node, ctx.facts);
    return ReadUnwrappedIdentifier(ctx.facts.Find(unwrapped));
}

static std::string_view ExtractNullComparedIdentifier(const NullFlowNode& node, const NullCheckContext& ctx)
{
    const NullFlowNode* rightNode = ctx.facts.Find(node.second);
    if (rightNode && rightNode->isNullInitializer)
    {
        return GetIdentifierName(node.first, ctx);
    }
    const NullFlowNode* leftNode = ctx.facts.Find(node.first);
    if (leftNode && leftNode->isNullInitializer)
    {
        return GetIdentifierName(node.second, ctx);
    }
    return {};
}

static NullConditionAssertions AssertionsFor(std::string_view name, bool isEquality)
{
    if (name.empty())
    {
        return {};
    }
    return {{{name, isEquality ? Nullability::DefinitelyNull : Nullability::NonNull}},
            {{name, isEquality ? Nullability::NonNull : Nullability::DefinitelyNull}}};
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

/**
 * @brief Computes facts and retains them for a subsequent enclosing condition.
 * @param[in] condition Condition whose facts are needed for short-circuit evaluation.
 * @return Facts valid until the next call to Take or Remember.
 */
const NullConditionAssertions& NullConditionWorklist::Remember(NullFlowNodeId condition)
{
    const NullFlowNodeId node = UnwrapNullExpression(condition, m_ctx.facts);
    if (node == NoNullFlowNode)
    {
        static const NullConditionAssertions empty;
        return empty;
    }
    auto assertions = Take(node);
    return m_summaries.try_emplace(node, std::move(assertions)).first->second;
}

/**
 * @brief Computes facts, consuming any already evaluated descendant summaries.
 * @param[in] condition Condition whose positive and negative facts are needed.
 * @return Assertions for both outcomes.
 */
NullConditionAssertions NullConditionWorklist::Take(NullFlowNodeId condition)
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

void NullConditionWorklist::EvaluateBinary(const NullFlowNode& node)
{
    if (!node.complete)
    {
        m_values.emplace_back();
        return;
    }
    if (node.op == NullFlowOperator::Is || node.op == NullFlowOperator::EqEq || node.op == NullFlowOperator::NotIs ||
        node.op == NullFlowOperator::NotEq)
    {
        const bool isEquality = (node.op == NullFlowOperator::Is || node.op == NullFlowOperator::EqEq);
        m_values.push_back(AssertionsFor(ExtractNullComparedIdentifier(node, m_ctx), isEquality));
        return;
    }
    const bool isAnd = (node.op == NullFlowOperator::And);
    if (isAnd || node.op == NullFlowOperator::Or)
    {
        m_pending.push_back({isAnd ? NullConditionAction::And : NullConditionAction::Or});
        m_pending.push_back({NullConditionAction::Evaluate, node.second});
        m_pending.push_back({NullConditionAction::Evaluate, node.first});
        return;
    }
    m_values.emplace_back();
}

void NullConditionWorklist::Evaluate(NullFlowNodeId condition)
{
    const NullFlowNodeId unwrapped = UnwrapNullExpression(condition, m_ctx.facts);
    if (unwrapped == NoNullFlowNode)
    {
        m_values.emplace_back();
        return;
    }
    if (const auto it = m_summaries.find(unwrapped); it != m_summaries.end())
    {
        m_values.push_back(std::move(it->second));
        m_summaries.erase(it);
        return;
    }
    const NullFlowNode* node = m_ctx.facts.Find(unwrapped);
    if (!node)
    {
        m_values.emplace_back();
        return;
    }
    if (node->kind == NullFlowNodeKind::Binary)
    {
        EvaluateBinary(*node);
    }
    else if (node->kind == NullFlowNodeKind::Unary)
    {
        if (node->op == NullFlowOperator::Not)
        {
            m_pending.push_back({NullConditionAction::Negate});
            m_pending.push_back({NullConditionAction::Evaluate, node->first});
        }
        else
        {
            m_values.emplace_back();
        }
    }
    else
    {
        m_values.push_back(AssertionsFor(ReadUnwrappedIdentifier(node), false));
    }
}

} // namespace angel_lsp::analysis
