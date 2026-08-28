#pragma once

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
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
