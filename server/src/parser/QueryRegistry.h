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
     * @brief Retrieves the precompiled binary expression query.
     * @return Immutable TSQuery pointer valid for the lifetime of the process.
     */
    [[nodiscard]] static const TSQuery* GetBinaryExpressionQuery();

    /**
     * @brief Retrieves the precompiled using declarations query.
     * @return Immutable TSQuery pointer valid for the lifetime of the process.
     */
    [[nodiscard]] static const TSQuery* GetUsingQuery();

    /**
     * @brief Retrieves a thread-local TSQueryCursor instance.
     * @return Reusable cursor unique to the calling thread.
     * @note Do not delete or share across threads.
     */
    [[nodiscard]] static TSQueryCursor* GetThreadLocalCursor();
};

/**
 * @brief Custom deleter for Tree-Sitter query pointers.
 */
struct QueryDeleter
{
    void operator()(TSQuery* q) const noexcept
    {
        if (q)
        {
            ts_query_delete(q);
        }
    }
};

/**
 * @brief RAII wrapper managing lifetime of a Tree-Sitter query cursor.
 */
struct CursorHolder
{
    TSQueryCursor* cursor = nullptr;

    CursorHolder()
    {
        cursor = ts_query_cursor_new();
    }

    ~CursorHolder()
    {
        if (cursor)
        {
            ts_query_cursor_delete(cursor);
            cursor = nullptr;
        }
    }

    CursorHolder(const CursorHolder&) = delete;
    CursorHolder& operator=(const CursorHolder&) = delete;
    CursorHolder(CursorHolder&&) = delete;
    CursorHolder& operator=(CursorHolder&&) = delete;
};
} // namespace angel_lsp::parser
