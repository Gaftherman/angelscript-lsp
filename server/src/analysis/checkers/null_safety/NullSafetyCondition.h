#pragma once

#include "analysis/checkers/null_safety/NullSafetyTypes.h"

#include <vector>

namespace angel_lsp::analysis
{
/// Positive and negative facts produced by one condition.
struct NullConditionAssertions
{
    std::vector<NullAssertion> positive;
    std::vector<NullAssertion> negative;
};

/// Follows captured expression links to find the value used for null tracking.
NullFlowNodeId UnwrapNullExpression(NullFlowNodeId expr, const NullFlowFacts& facts);

/// Extracts a simple identifier using the captured expression operands.
std::string_view GetIdentifierName(NullFlowNodeId node, const NullCheckContext& ctx);

/// Worklist actions for null checking.
enum class NullConditionAction : uint8_t
{
    Evaluate,
    Negate,
    And,
    Or
};

/// A pending step in the null checking worklist.
struct NullConditionWork
{
    NullConditionAction action;
    NullFlowNodeId node = NoNullFlowNode;
};

/**
 * @brief Computes null facts over indexed operands and reuses them during one expression check.
 * @note The worklist and its transient node keys are confined to one live revision.
 */
class NullConditionWorklist
{
  public:
    /** @brief Initializes an empty worklist for one expression. */
    explicit NullConditionWorklist(const NullCheckContext& ctx) : m_ctx(ctx)
    {
    }

    /**
     * @brief Computes facts and retains them for a subsequent enclosing condition.
     * @param[in] condition Condition whose facts are needed for short-circuit evaluation.
     * @return Facts valid until the next call to Take or Remember.
     */
    const NullConditionAssertions& Remember(NullFlowNodeId condition);

    /**
     * @brief Computes facts, consuming any already evaluated descendant summaries.
     * @param[in] condition Condition whose positive and negative facts are needed.
     * @return Assertions for both outcomes.
     */
    NullConditionAssertions Take(NullFlowNodeId condition);

  private:
    void Merge(bool isAnd);
    void EvaluateBinary(const NullFlowNode& node);
    void Evaluate(NullFlowNodeId condition);

    const NullCheckContext& m_ctx;
    std::vector<NullConditionWork> m_pending;
    std::vector<NullConditionAssertions> m_values;
    ankerl::unordered_dense::map<NullFlowNodeId, NullConditionAssertions> m_summaries;
};

} // namespace angel_lsp::analysis
