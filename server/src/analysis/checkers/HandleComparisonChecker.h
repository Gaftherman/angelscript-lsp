#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Validates handle equality and relational operations in binary expressions.
 * @param[in] node Binary expression syntax node.
 * @param[in] scope Lexical scope enclosing the expression.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckHandleComparison(TSNode node, const Scope* scope, DiagnosticContext& ctx);

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
 * @brief Carries AST node, operand types, and symbol table state during handle comparison rule evaluation.
 */
struct ComparisonDispatchContext
{
    TSNode opNode;
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
    std::string leftType;
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
    std::string leftType;
};
} // namespace angel_lsp::analysis
