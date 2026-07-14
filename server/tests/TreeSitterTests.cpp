#include <doctest/doctest.h>
#include <tree_sitter/api.h>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

extern "C" TSLanguage* tree_sitter_angelscript();

struct Parser {
    TSParser* raw = nullptr;
    Parser() {
        raw = ts_parser_new();
        ts_parser_set_language(raw, tree_sitter_angelscript());
    }
    ~Parser() { ts_parser_delete(raw); }
    TSTree* parse(const std::string& code, TSTree* oldTree = nullptr) const {
        return ts_parser_parse_string(raw, oldTree, code.c_str(), (uint32_t)code.size());
    }
};

struct Tree {
    TSTree* raw = nullptr;
    explicit Tree(TSTree* t) : raw(t) {}
    ~Tree() { if (raw) ts_tree_delete(raw); }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    TSNode root() const { return ts_tree_root_node(raw); }
};

TEST_SUITE("TreeSitter - Basic Parsing") {
    TEST_CASE("Simple void function") {
        Parser p;
        Tree tree(p.parse("void Main() { }"));
        TSNode root = tree.root();
        CHECK_FALSE(ts_node_has_error(root));
        REQUIRE(ts_node_named_child_count(root) == 1);
        TSNode func = ts_node_named_child(root, 0);
        CHECK(std::string(ts_node_type(func)) == "func_declaration");
    }
}
