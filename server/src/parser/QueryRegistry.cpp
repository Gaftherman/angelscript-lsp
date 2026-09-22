#include "parser/QueryRegistry.h"
#include "parser/queries/BuiltQueries.h"

#include <cstring>
#include <memory>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::parser
{
namespace
{
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

using UniqueTSQuery = std::unique_ptr<TSQuery, QueryDeleter>;

UniqueTSQuery CompileQuery(const char* source)
{
    uint32_t errorOffset = 0;
    TSQueryError errorType = TSQueryErrorNone;
    const TSLanguage* lang = tree_sitter_angelscript();
    TSQuery* query = ts_query_new(lang, source, static_cast<uint32_t>(std::strlen(source)), &errorOffset, &errorType);
    return UniqueTSQuery(query);
}

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
} // namespace

const TSQuery* QueryRegistry::GetHighlightsQuery()
{
    static const UniqueTSQuery s_query = CompileQuery(queries::HIGHLIGHTS_QUERY);
    return s_query.get();
}

const TSQuery* QueryRegistry::GetLocalsQuery()
{
    static const UniqueTSQuery s_query = CompileQuery(queries::LOCALS_QUERY);
    return s_query.get();
}

const TSQuery* QueryRegistry::GetTagsQuery()
{
    static const UniqueTSQuery s_query = CompileQuery(queries::TAGS_QUERY);
    return s_query.get();
}

TSQueryCursor* QueryRegistry::GetThreadLocalCursor()
{
    thread_local CursorHolder t_holder;
    return t_holder.cursor;
}

} // namespace angel_lsp::parser
