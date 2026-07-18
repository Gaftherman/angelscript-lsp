#pragma once

#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <unordered_map>

/**
 * @brief Deleter for Tree-Sitter queries to be used with smart pointers.
 */
struct TSQueryDeleter
{
    void operator()(TSQuery *q) const
    {
        if (q) ts_query_delete(q);
    }
};

/**
 * @brief Deleter for Tree-Sitter query cursors to be used with smart pointers.
 */
struct TSQueryCursorDeleter
{
    void operator()(TSQueryCursor *qc) const
    {
        if (qc) ts_query_cursor_delete(qc);
    }
};

/**
 * @brief Represents a single match from a Tree-Sitter query execution.
 */
struct QueryMatch
{
    uint32_t pattern_index;
    std::unordered_map<std::string, TSNode> captures;
};

/**
 * @brief A singleton utility to compile, store, and execute Tree-Sitter queries.
 * 
 * Centralizes query execution to avoid recompiling the same query strings repeatedly.
 */
class QueryRunner
{
public:
    /**
     * @brief Gets the singleton instance of the QueryRunner.
     * 
     * @return The QueryRunner instance.
     */
    static QueryRunner &GetInstance();

    /**
     * @brief Compiles and registers a Tree-Sitter query.
     * 
     * If the query is already registered, it does nothing and returns true.
     * 
     * @param name The unique name to assign to the query.
     * @param source The Tree-Sitter query string.
     * @param out_error Output parameter that will contain the parse error if compilation fails.
     * @return true if the query was compiled or already exists, false on syntax error.
     */
    bool RegisterQuery(const std::string &name, const std::string &source, std::string &out_error);

    /**
     * @brief Executes a previously registered query against a given syntax node.
     * 
     * @param name The name of the registered query.
     * @param root_node The Tree-Sitter node to query against.
     * @return A list of query matches and their named captures.
     */
    std::vector<QueryMatch> ExecuteQuery(const std::string &name, TSNode root_node);

private:
    QueryRunner() = default;
    ~QueryRunner() = default;

    std::unordered_map<std::string, std::unique_ptr<TSQuery, TSQueryDeleter>> queries;
};
