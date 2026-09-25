#pragma once

#include "analysis/NullSafetyTypes.h"
#include <string_view>
#include <vector>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Unwraps parenthesized or handle-operator unary nodes to find the core expression.
 * @param[in] expr Expression AST node to unwrap.
 * @return Unwrapped child AST node.
 */
TSNode UnwrapNullExpression(TSNode expr);

/**
 * @brief Extracts the simple identifier name from an identifier or scoped_identifier node.
 * @param[in] node AST node to inspect.
 * @param[in] sourceCode Source text of the document.
 * @return Simple variable or parameter identifier name, or empty string.
 */
std::string GetIdentifierName(TSNode node, std::string_view sourceCode);

/**
 * @brief Evaluates an AST condition node to extract positive and negative null assertions.
 * @param[in] condition Condition AST node (e.g. from if_statement or while_statement).
 * @param[in] sourceCode Source text of the document.
 * @param[out] positiveAssertions Assertions that hold if the condition is true.
 * @param[out] negativeAssertions Assertions that hold if the condition is false.
 */
void ExtractConditionAssertions(TSNode condition,
                                std::string_view sourceCode,
                                std::vector<NullAssertion>& positiveAssertions,
                                std::vector<NullAssertion>& negativeAssertions);

} // namespace angel_lsp::analysis
