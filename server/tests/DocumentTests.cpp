#include <doctest/doctest.h>
#include <iostream>
#include "document/Document.h"

TEST_SUITE("Document - Incremental Parsing") {
    TEST_CASE("Document parses correctly on init") {
        Document doc("file:///test.as", "void Main() { }");
        TSNode root = doc.RootNode();
        CHECK_FALSE(ts_node_has_error(root));
        CHECK(ts_node_named_child_count(root) == 1);
    }

    TEST_CASE("Document updates tree on edit") {
        Document doc("file:///test.as", "void Main() { }");
        
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

    TEST_CASE("Document returns correct node at position") {
        Document doc("file:///test.as", "void Main() { int myVar = 10; }");
        // 'myVar' starts at line 0, col 18
        TSNode node = doc.NodeAt(0, 18);
        CHECK_FALSE(ts_node_is_null(node));
        CHECK(std::string(ts_node_type(node)) == "identifier");
        
        std::string_view source = doc.SourceAt(node);
        CHECK(source == "myVar");
    }
}
