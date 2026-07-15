#include <doctest/doctest.h>
#include "analysis/SymbolCollector.h"
#include "document/Document.h"
#include "helpers/TestFixtures.h"

using namespace analysis;

// Helper to dump AST (for debugging)
static void DumpAST(TSNode node, int depth = 0)
{
    if (ts_node_is_null(node)) return;
    std::string indent(depth * 2, ' ');
    printf("%s%s\n", indent.c_str(), ts_node_type(node));
    for (uint32_t i = 0; i < ts_node_child_count(node); i++)
    {
        DumpAST(ts_node_child(node, i), depth + 1);
    }
}

TEST_SUITE("SymbolCollector")
{
    TEST_CASE("Collect globals finds functions and variables")
    {
        Document doc("file:///test.as", "void Main() { int x = 5; } int g_var = 10;");
        SymbolTable table;
        
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* mainFunc = table.FindGlobalByName("Main");
        REQUIRE(mainFunc != nullptr);
        CHECK(mainFunc->kind == SymbolKind::Function);
        CHECK(mainFunc->typeInfo == "void");
        CHECK(mainFunc->selectionRange.start.line == 0);
        
        Symbol* globalVar = table.FindGlobalByName("g_var");
        REQUIRE(globalVar != nullptr);
        CHECK(globalVar->kind == SymbolKind::Variable);
        CHECK(globalVar->typeInfo == "int");
    }

    TEST_CASE("Collect globals finds classes, namespaces and their members")
    {
        Document doc("file:///test.as", "namespace Math { int PI = 3; } class Player { int hp; void Heal() {} }");
        SymbolTable table;
        
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* mathNs = table.FindGlobalByName("Math");
        REQUIRE(mathNs != nullptr);
        CHECK(mathNs->kind == SymbolKind::Namespace);
        REQUIRE(mathNs->children.size() == 1);
        CHECK(mathNs->children[0]->name == "PI");
        CHECK(mathNs->children[0]->kind == SymbolKind::Variable);
        
        Symbol* playerCls = table.FindGlobalByName("Player");
        REQUIRE(playerCls != nullptr);
        CHECK(playerCls->kind == SymbolKind::Class);
        REQUIRE(playerCls->children.size() == 2);
        
        // Members might be out of order depending on AST, but usually in order:
        CHECK(playerCls->children[0]->name == "hp");
        CHECK(playerCls->children[0]->kind == SymbolKind::Variable);
        
        CHECK(playerCls->children[1]->name == "Heal");
        CHECK(playerCls->children[1]->kind == SymbolKind::Function);
    }

    TEST_CASE("Collect globals finds enums, funcdefs, and virtual properties")
    {
        Document doc("file:///test.as", "enum State { IDLE, RUN } funcdef void Callback(); class Window { int width { get { return 0; } } }");
        SymbolTable table;
        
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* stateEnum = table.FindGlobalByName("State");
        REQUIRE(stateEnum != nullptr);
        CHECK(stateEnum->kind == SymbolKind::Enum);
        
        Symbol* callbackFd = table.FindGlobalByName("Callback");
        REQUIRE(callbackFd != nullptr);
        CHECK(callbackFd->kind == SymbolKind::Funcdef);
        CHECK(callbackFd->typeInfo == "void");
        
        Symbol* windowCls = table.FindGlobalByName("Window");
        REQUIRE(windowCls != nullptr);
        REQUIRE(windowCls->children.size() == 1);
        
        Symbol* widthProp = windowCls->children[0].get();
        CHECK(widthProp->name == "width");
        CHECK(widthProp->kind == SymbolKind::Property);
        CHECK(widthProp->typeInfo == "int");
    }

    TEST_CASE("Collect locals finds local variables")
    {
        Document doc("file:///test.as", "void Main() { int local_var = 5; float another = 2.0f; }");
        SymbolTable table;
        
        // Find the statement_block inside Main to traverse
        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 0); // func_declaration
        TSNode blockNode = ts_node_child(funcNode, 3); // statement_block
        
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);
        
        const auto& locals = table.GetLocals();
        REQUIRE(locals.size() == 2);
        CHECK(locals[0]->name == "local_var");
        CHECK(locals[0]->typeInfo == "int");
        CHECK(locals[1]->name == "another");
        CHECK(locals[1]->typeInfo == "float");
    }

    TEST_CASE("Collect globals finds nested namespaces")
    {
        Document doc("file:///test.as", "namespace Engine { namespace Math { float Lerp(float a, float b, float t) { return a; } } }");
        SymbolTable table;
        
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* engineNs = table.FindGlobalByName("Engine");
        REQUIRE(engineNs != nullptr);
        CHECK(engineNs->kind == SymbolKind::Namespace);
        REQUIRE(engineNs->children.size() == 1);
        
        Symbol* mathNs = engineNs->children[0].get();
        CHECK(mathNs->name == "Math");
        CHECK(mathNs->kind == SymbolKind::Namespace);
        REQUIRE(mathNs->children.size() == 1);
        
        Symbol* lerpFunc = mathNs->children[0].get();
        CHECK(lerpFunc->name == "Lerp");
        CHECK(lerpFunc->kind == SymbolKind::Function);
        CHECK(lerpFunc->typeInfo == "float");
    }
}
