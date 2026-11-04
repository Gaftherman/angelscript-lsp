#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"

#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Checks for repeated implicit conversions in conditional statement ladders.
 * @param[in] rootIfNode The root if_statement syntax node.
 * @param[in] scope Lexical scope of the conditional statement.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckRepeatedConversions(TSNode rootIfNode, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Collects condition handles along the alternative links of an if-else ladder.
 * @param[in] rootIf First conditional statement.
 * @param[in] index Extracted syntax for diagnostics, or nullptr for an interactive query.
 * @return Nonempty conditions in source order, excluding the final else body.
 */
[[nodiscard]] std::vector<TSNode> CollectIfLadderConditions(TSNode rootIf, const NodeIndex* index);

/**
 * @brief Records source line and target type details for repeated type conversion diagnostic reporting.
 */
struct ConversionOccurrence
{
    TSNode node;
    std::string exprText;
    std::string fromType;
    std::string toType;
};
} // namespace angel_lsp::analysis
