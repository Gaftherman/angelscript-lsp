#include <doctest/doctest.h>
#include <iostream>
#include "helpers/TestFixtures.h"
#include <spdlog/spdlog.h>

TEST_SUITE("Document - Incremental Parsing")
{
    TEST_CASE("Document parses correctly on init")
    {
        std::string code = "void Main() { }";
        Document doc = fixtures::MakeDoc(code);
        TSNode root = doc.RootNode();

        spdlog::info("Document parses correctly on init:");
        spdlog::info("-> Code: {}", code);
        spdlog::info("-> Root Node Type: {}", ts_node_type(root));
        spdlog::info("-> Root Child Count: {}", ts_node_named_child_count(root));

        CHECK_FALSE(ts_node_has_error(root));
        CHECK(ts_node_named_child_count(root) == 1);
    }

    TEST_CASE("Document updates tree on edit")
    {
        Document doc = fixtures::MakeDoc("void Main() { }");

        // Insert 'int x = 42;' inside the braces (at byte offset 13)
        lsp::TextEdit edit;
        edit.range.start = {0, 13};
        edit.range.end = {0, 13};
        edit.newText = "\n    int x = 42;\n";

        doc.ApplyEdit(edit);

        TSNode root = doc.RootNode();
        CHECK_FALSE(ts_node_has_error(root));

        std::string expectedText = "void Main() {\n    int x = 42;\n }";
        CHECK(doc.GetText() == expectedText);
    }

    TEST_CASE("Document returns correct node at position")
    {
        std::string code = "void Main() { int myVar = 10; }";
        Document doc = fixtures::MakeDoc(code);
        // 'myVar' starts at line 0, col 18
        TSNode node = doc.NodeAt(0, 18);

        spdlog::info("Document returns correct node at position:");
        spdlog::info("-> Requesting node at Line 0, Col 18 in: {}", code);

        CHECK_FALSE(ts_node_is_null(node));

        std::string nodeType = ts_node_type(node);
        std::string_view source = doc.SourceAt(node);

        spdlog::info("-> Detected Node Type: {}", nodeType);
        spdlog::info("-> Detected Node Source: {}", source);

        CHECK(nodeType == "identifier");
        CHECK(source == "myVar");
    }
}
