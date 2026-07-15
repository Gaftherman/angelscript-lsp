#pragma once

#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <unordered_map>

struct TSQueryDeleter {
    void operator()(TSQuery* q) const { if(q) ts_query_delete(q); }
};

struct TSQueryCursorDeleter {
    void operator()(TSQueryCursor* qc) const { if(qc) ts_query_cursor_delete(qc); }
};

struct QueryMatch {
    uint32_t pattern_index;
    std::unordered_map<std::string, TSNode> captures;
};

class QueryRunner {
public:
    static QueryRunner& GetInstance();

    // Compila y almacena una query. Si ya existe, no hace nada.
    bool RegisterQuery(const std::string& name, const std::string& source, std::string& out_error);

    // Ejecuta una query registrada sobre un nodo (ej. la raíz del árbol).
    std::vector<QueryMatch> ExecuteQuery(const std::string& name, TSNode root_node);

private:
    QueryRunner() = default;
    ~QueryRunner() = default;

    std::unordered_map<std::string, std::unique_ptr<TSQuery, TSQueryDeleter>> queries;
};
