#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/ScopeTree.h"

#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Validates handle equality and relational operations in binary expressions.
 * @param[in] fact Binary expression facts with operator and both operands.
 * @param[in] scope Lexical scope enclosing the expression.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckHandleComparison(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Category of comparison expression evaluated during semantic analysis.
 */
enum class ComparisonCategory
{
    None,
    ValueEquality,
    Relational
};

/**
 * @brief Encapsulates operator symbol identifier, method name, and symmetry flags for handle comparison dispatch.
 */
struct ComparisonOperatorInfo
{
    ComparisonCategory category = ComparisonCategory::None;
    std::string_view identityEquivalent;
};

/**
 * @brief Carries operator range, spelling and classification during handle comparison rule evaluation.
 */
struct ComparisonDispatchContext
{
    SourceRange opRange;
    std::string_view op;
    ComparisonOperatorInfo opInfo;
};

/**
 * @brief Holds left and right operand expressions and typing metadata for relational comparison checks.
 */
struct RelationalCheckOperands
{
    bool hasNull = false;
    bool bothHandles = false;
    std::string_view leftType; ///< Resolved left operand type; owned by the caller for the duration of the check.
};

/**
 * @brief Tracks evaluation state and resolved types for operands in an equality or relational comparison expression.
 */
struct ComparisonOperandsState
{
    bool isLeftNull = false;
    bool isRightNull = false;
    bool isLeftHandle = false;
    bool isRightHandle = false;
    std::string_view leftType; ///< Resolved left operand type; owned by the caller for the duration of the check.
};
} // namespace angel_lsp::analysis
