#pragma once

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Hard ceiling on how deep any recursive walk over a syntax tree may go.
     *
     * Tree-sitter imposes no depth limit and happily builds a tree as deeply nested as the source
     * is, so a document consisting of a few thousand `(` characters - well under any reasonable
     * size limit - produced a tree deep enough to overflow the stack in every checker that walks it
     * recursively. There are a dozen such walks per analysis and each one is a separate crash.
     *
     * Source nested past this is pathological by definition, and abandoning a subtree costs only a
     * diagnostic - the safe direction for this analyzer, whose stated policy is that a missed error
     * costs nothing and a false one costs the user's trust in every other diagnostic on screen.
     * SymbolCollector::ReportParseErrors already had its own cap for exactly this reason; this is
     * that idea applied everywhere it was missing.
     *
     * The number is low on purpose, and was measured rather than guessed: 512, 256 and 128 all
     * still overflowed, and 64 is the first value that survives. Two things make the usable budget
     * far smaller than the raw stack size suggests:
     *
     *  - The frames are large. The expression resolvers hold several std::string locals apiece.
     *  - The caps compose. A guarded checker recursing 64 deep calls a resolver that then begins
     *    its own 64-deep budget, so the real worst case is a multiple of this number.
     *
     * Debug frames are larger than Release ones and this has to hold in the build developers
     * actually run, so the limit is set for the worse case.
     */
    inline constexpr int k_maxAstDepth = 64;

    /**
     * @brief Returns the AST node type name as a zero-allocation string view.
     * @param node The TSNode to inspect.
     * @return String view containing the node type name.
     */
    [[nodiscard]] inline std::string_view NodeType(TSNode node) noexcept
    {
        if (ts_node_is_null(node))
        {
            return {};
        }
        const char *t = ts_node_type(node);
        return t ? std::string_view(t) : std::string_view{};
    }

    /**
     * @brief Extracts the raw source text corresponding to an AST node as a string view.
     * @param node The TSNode whose slice to extract.
     * @param sourceCode The full document source code.
     * @return Slice of sourceCode spanning the node's byte range.
     */
    [[nodiscard]] inline std::string_view NodeText(TSNode node, std::string_view sourceCode) noexcept
    {
        if (ts_node_is_null(node))
        {
            return {};
        }
        const uint32_t start = ts_node_start_byte(node);
        const uint32_t end = ts_node_end_byte(node);
        if (start >= sourceCode.size() || end > sourceCode.size() || start >= end)
        {
            return {};
        }
        return sourceCode.substr(start, end - start);
    }

    /**
     * @brief Trims leading and trailing ASCII whitespace from a string view.
     * @param text The input string view.
     * @return The trimmed string view.
     */
    [[nodiscard]] inline std::string_view Trim(std::string_view text) noexcept
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        {
            text.remove_suffix(1);
        }
        return text;
    }
}
