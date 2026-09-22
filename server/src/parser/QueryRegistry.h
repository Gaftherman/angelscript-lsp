#pragma once

#include <tree_sitter/api.h>

namespace angel_lsp::parser
{
/**
 * @brief Central thread-safe registry providing precompiled Tree-Sitter queries and thread-local cursors.
 */
class QueryRegistry
{
  public:
    /**
     * @brief Retrieves the precompiled highlights query.
     * @return Immutable TSQuery pointer valid for the lifetime of the process.
     */
    [[nodiscard]] static const TSQuery* GetHighlightsQuery();

    /**
     * @brief Retrieves the precompiled local scopes query.
     * @return Immutable TSQuery pointer valid for the lifetime of the process.
     */
    [[nodiscard]] static const TSQuery* GetLocalsQuery();

    /**
     * @brief Retrieves the precompiled tags query for symbol indexing.
     * @return Immutable TSQuery pointer valid for the lifetime of the process.
     */
    [[nodiscard]] static const TSQuery* GetTagsQuery();

    /**
     * @brief Retrieves a thread-local TSQueryCursor instance.
     * @return Reusable cursor unique to the calling thread.
     * @note Do not delete or share across threads.
     */
    [[nodiscard]] static TSQueryCursor* GetThreadLocalCursor();
};
} // namespace angel_lsp::parser
