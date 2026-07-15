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
        printf("ts_parser_new returned %p\n", raw);
        TSLanguage* lang = tree_sitter_angelscript();
        printf("tree_sitter_angelscript returned %p\n", lang);
        bool ok = ts_parser_set_language(raw, lang);
        printf("ts_parser_set_language returned %d\n", ok);
    }
    ~Parser() { ts_parser_delete(raw); }
    TSTree* parse(const std::string& code, TSTree* oldTree = nullptr) const {
        printf("parsing string...\n");
        TSTree* res = ts_parser_parse_string(raw, oldTree, code.c_str(), (uint32_t)code.size());
        printf("parsed string, res=%p\n", res);
        return res;
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

    TEST_CASE("AST dump: function inside nested namespace") {
        Parser p;
        std::string code =
            "namespace Engine {\n"
            "    namespace Math {\n"
            "        float Lerp(float a, float b, float t) { return a + (b - a) * t; }\n"
            "    }\n"
            "}\n";
        Tree tree(p.parse(code));
        TSNode root = tree.root();

        std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth) {
            if (ts_node_is_null(node)) return;
            std::string indent(depth * 2, ' ');
            uint32_t start = ts_node_start_byte(node);
            uint32_t end   = ts_node_end_byte(node);
            std::string src = code.substr(start, end - start);
            if (src.size() > 30) src = src.substr(0, 27) + "...";
            // Escape newlines for readability
            for (auto& ch : src) if (ch == '\n') ch = ' ';
            printf("%s[%s] \"%s\"\n", indent.c_str(), ts_node_type(node), src.c_str());
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                printTree(ts_node_child(node, i), depth + 1);
        };

        printf("\n=== AST: function inside nested namespace ===\n");
        printTree(root, 0);
        printf("=== END AST ===\n\n");

        // This test always passes — its purpose is to print the AST structure
        // so we can identify the exact node type names used for function parameters
        // inside a namespace (e.g. "parameter_list" vs "parameter_list_decl").
        CHECK_FALSE(ts_node_is_null(root));
    }

    TEST_CASE("AST dump: mixin class body and host class body") {
        Parser p;
        std::string code =
            "class Entity { float hp; float speed; }\n"
            "mixin class Regenerator {\n"
            "    float regenRate;\n"
            "    void Regen() { hp = hp + regenRate; }\n"
            "}\n"
            "class Troll : Entity, Regenerator {\n"
            "    int angerLevel;\n"
            "    void Enrage() { regenRate = 5.0f; }\n"
            "}\n";
        Tree tree(p.parse(code));
        TSNode root = tree.root();

        std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth) {
            if (ts_node_is_null(node)) return;
            std::string indent(depth * 2, ' ');
            uint32_t start = ts_node_start_byte(node);
            uint32_t end   = ts_node_end_byte(node);
            std::string src = code.substr(start, end - start);
            if (src.size() > 50) src = src.substr(0, 47) + "...";
            for (auto& ch : src) if (ch == '\n') ch = ' ';
            // Only print named nodes to reduce noise
            if (ts_node_is_named(node))
                printf("%s[%s] \"%s\"\n", indent.c_str(), ts_node_type(node), src.c_str());
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                printTree(ts_node_child(node, i), depth + 1);
        };

        printf("\n=== AST: mixin class + host class body ===\n");
        printTree(root, 0);
        printf("=== END AST ===\n\n");

        CHECK_FALSE(ts_node_is_null(root));
    }

    TEST_CASE("AST dump: broken namespace from user") {
        Parser p;
        std::string code = 
            "namespace Engine {\n"
            "    namespace Math {\n"
            "        float Lerp(float a, float b, float t) { // El hover lo detecta como variable \n"
            "            return a + (b - a) * t;\n"
            "        }\n"
            "}";
        Tree tree(p.parse(code));
        TSNode root = tree.root();

        std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth) {
            if (ts_node_is_null(node)) return;
            std::string indent(depth * 2, ' ');
            uint32_t start = ts_node_start_byte(node);
            uint32_t end   = ts_node_end_byte(node);
            std::string src = code.substr(start, end - start);
            if (src.size() > 50) src = src.substr(0, 47) + "...";
            for (auto& ch : src) if (ch == '\n') ch = ' ';
            if (ts_node_is_named(node))
                printf("%s[%s] \"%s\"\n", indent.c_str(), ts_node_type(node), src.c_str());
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                printTree(ts_node_child(node, i), depth + 1);
        };

        printf("\n=== AST: broken namespace ===\n");
        printTree(root, 0);
        printf("=== END AST ===\n\n");
    }

    TEST_CASE("AST dump: local var with args") {
        Parser p;
        std::string code = "void Main() { Vector3 v(1, 2, 3); }";
        Tree tree(p.parse(code));
        TSNode root = tree.root();

        std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth) {
            if (ts_node_is_null(node)) return;
            std::string indent(depth * 2, ' ');
            uint32_t start = ts_node_start_byte(node);
            uint32_t end   = ts_node_end_byte(node);
            std::string src = code.substr(start, end - start);
            if (src.size() > 50) src = src.substr(0, 47) + "...";
            for (auto& ch : src) if (ch == '\n') ch = ' ';
            if (ts_node_is_named(node))
                printf("%s[%s] \"%s\"\n", indent.c_str(), ts_node_type(node), src.c_str());
            else
                printf("%s(%s)\n", indent.c_str(), ts_node_type(node));
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                printTree(ts_node_child(node, i), depth + 1);
        };

        printf("\n=== AST: local var with args ===\n");
        printTree(root, 0);
        printf("=== END AST ===\n\n");
    }
}
