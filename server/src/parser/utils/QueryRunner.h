/**
 * @file QueryRunner.h
 * @brief Singleton utility for compiling, caching, and executing Tree-Sitter queries.
 * @ingroup Parser
 */

#pragma once

#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <unordered_map>

/**
 * @brief Custom RAII deleter for Tree-Sitter TSQuery pointers.
 */
struct TSQueryDeleter
{
    void operator()(TSQuery *q) const
    {
        if (q)
            ts_query_delete(q);
    }
};

/**
 * @brief Custom RAII deleter for Tree-Sitter TSQueryCursor pointers.
 */
struct TSQueryCursorDeleter
{
    void operator()(TSQueryCursor *qc) const
    {
        if (qc)
            ts_query_cursor_delete(qc);
    }
};

/**
 * @brief Represents a single match result from a Tree-Sitter query execution.
 */
struct QueryMatch
{
    uint32_t pattern_index;
    std::unordered_map<std::string, TSNode> captures;
};

/**
 * @brief A singleton utility class to compile, cache, and execute Tree-Sitter queries.
 * @note Centralizes query execution to avoid recompiling query strings repeatedly.
 */
class QueryRunner
{
public:
    /**
     * @brief Gets the singleton instance of the QueryRunner.
     * @return QueryRunner& Reference to the global QueryRunner instance.
     */
    static QueryRunner &GetInstance();

    /**
     * @brief Compiles and registers a Tree-Sitter query.
     *
     * @param[in] name The unique name to assign to the query.
     * @param[in] source The Tree-Sitter pattern query string.
     * @param[out] out_error Output parameter containing error message if compilation fails.
     * @return bool True if query was compiled or already exists; false on syntax error.
     */
    bool RegisterQuery(const std::string &name, const std::string &source, std::string &out_error);

    /**
     * @brief Executes a registered query against a syntax node.
     *
     * @param[in] name The name of the registered query.
     * @param[in] root_node The Tree-Sitter syntax node to query against.
     * @return std::vector<QueryMatch> A vector of query matches and named captures.
     */
    std::vector<QueryMatch> ExecuteQuery(const std::string &name, TSNode root_node);

private:
    QueryRunner() = default;
    ~QueryRunner() = default;

    std::unordered_map<std::string, std::unique_ptr<TSQuery, TSQueryDeleter>> queries;
};
