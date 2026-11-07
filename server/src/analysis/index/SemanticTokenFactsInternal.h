#pragma once

#include "analysis/index/SemanticTokenFacts.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Tree-sitter query capture rule mapping capture names to LSP semantic token types.
 */
struct CaptureRule
{
    uint32_t tokenType = Type_Variable;
    uint32_t tokenMod = 0;
    int priority = 1;
    bool isOperatorOrPunctuation = false;
    bool valid = false;
};

/**
 * @brief Pre-compiled query and token mapping tables for syntax highlighting.
 */
struct HighlightsQueryData
{
    TSQuery* query = nullptr;
    std::vector<CaptureRule> rules;
};

/**
 * @brief Classifies an operator or punctuation capture node into token type and priority.
 * @param[in] node AST capture node.
 * @param[in] sourceCode Document source text.
 * @param[in] syms Cached grammar symbols.
 * @return Optional pair of (tokenType, priority), or nullopt when the capture is not highlighted.
 */
[[nodiscard]] std::optional<std::pair<uint32_t, int>>
ClassifyOperatorOrPunctuation(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& syms) noexcept;

/**
 * @brief Checks whether an AST node is the angle bracket of a template argument or parameter list.
 * @param[in] node AST node to check.
 * @param[in] syms Grammar symbols table.
 * @return True if node is template punctuation.
 */
[[nodiscard]] bool IsTemplatePunctuationNode(TSNode node, const parser::GrammarSymbols& syms) noexcept;

/**
 * @brief Returns the compiled highlights query and the token rule for each of its captures.
 * @return Reference to the lazily built process-wide HighlightsQueryData.
 */
[[nodiscard]] const HighlightsQueryData& GetHighlightsQueryData();

} // namespace angel_lsp::analysis
