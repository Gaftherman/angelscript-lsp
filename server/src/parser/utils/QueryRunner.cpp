/**
 * @file QueryRunner.cpp
 * @brief Implementation of QueryRunner singleton for Tree-Sitter query compilation and execution.
 * @ingroup Parser
 */

#include "parser/utils/QueryRunner.h"
#include "utils/LspLogger.h"

extern "C" const TSLanguage *tree_sitter_angelscript();

QueryRunner &QueryRunner::GetInstance()
{
    static QueryRunner instance;
    return instance;
}

bool QueryRunner::RegisterQuery(const std::string &name, const std::string &source, std::string &out_error)
{
    if (queries.find(name) != queries.end())
    {
        return true; // Already registered
    }

    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;

    TSQuery *query = ts_query_new(
        tree_sitter_angelscript(),
        source.c_str(),
        source.length(),
        &error_offset,
        &error_type);

    if (!query)
    {
        out_error = "Failed to parse query at offset " + std::to_string(error_offset) +
                    ", error code: " + std::to_string(error_type);
        angel_lsp::LspLogger::Error("[QueryRunner] " + out_error);
        return false;
    }

    queries[name] = std::unique_ptr<TSQuery, TSQueryDeleter>(query);
    angel_lsp::LspLogger::Info("[QueryRunner] Successfully registered query '" + name + "'");
    return true;
}

std::vector<QueryMatch> QueryRunner::ExecuteQuery(const std::string &name, TSNode root_node)
{
    std::vector<QueryMatch> results;

    auto it = queries.find(name);
    if (it == queries.end())
    {
        angel_lsp::LspLogger::Warn("[QueryRunner] Attempted to execute unregistered query '" + name + "'");
        return results;
    }

    TSQuery *query = it->second.get();
    std::unique_ptr<TSQueryCursor, TSQueryCursorDeleter> cursor(ts_query_cursor_new());

    ts_query_cursor_exec(cursor.get(), query, root_node);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor.get(), &match))
    {
        QueryMatch qm;
        qm.pattern_index = match.pattern_index;

        for (uint16_t i = 0; i < match.capture_count; i++)
        {
            const TSQueryCapture &capture = match.captures[i];

            uint32_t name_len = 0;
            const char *capture_name = ts_query_capture_name_for_id(query, capture.index, &name_len);

            if (capture_name && name_len > 0)
            {
                std::string name_str(capture_name, name_len);
                qm.captures[name_str] = capture.node;
            }
        }

        results.push_back(std::move(qm));
    }

    return results;
}
