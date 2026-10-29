#pragma once

#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace angel_lsp::features
{

/**
 * @brief Represents an evaluatable expression range and text extracted from AST.
 */
struct EvaluatableExpressionItem
{
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t endLine = 0;
    uint32_t endColumn = 0;
    std::string expression;
};

/**
 * @brief Request parameters for evaluatable expression resolution.
 */
struct EvaluatableExpressionRequest
{
    TSNode rootNode{};
    std::string_view sourceCode;
    uint32_t line = 0;
    uint32_t column = 0;
};

/**
 * @brief AST-aware evaluatable expression provider for debugging hovers.
 */
class EvaluatableExpressionHandler
{
  public:
    /**
     * @brief Extracts the evaluatable expression under the cursor using Tree-Sitter AST.
     * @param[in] request Immutable request containing AST root, source text, and position.
     * @return Evaluatable expression item, or std::nullopt if position is not evaluatable.
     */
    static std::optional<EvaluatableExpressionItem> Extract(const EvaluatableExpressionRequest& request);
};

} // namespace angel_lsp::features
