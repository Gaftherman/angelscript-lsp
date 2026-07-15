#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"
#include "analysis/SymbolResolver.h"
#include "analysis/SymbolCollector.h"

using namespace analysis;

TEST_SUITE("SymbolResolver")
{
    TEST_CASE("ResolveAt basic global resolution")
    {
        Document doc("file:///test.as", "void TargetFunc() {} void Main() { TargetFunc(); }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        // Find position of 'TargetFunc' in the body of Main
        // "void TargetFunc() {} void Main() { TargetFunc(); }"
        //  012345678901234567890123456789012345678901234
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, 37);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "TargetFunc");
        CHECK(sym->kind == SymbolKind::Function);
    }

    TEST_CASE("ResolveAt member expression")
    {
        std::string code = "class Player { int hp; } void Main() { Player player; player.hp; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        auto playerVar = std::make_shared<Symbol>();
        playerVar->name = "player";
        playerVar->kind = SymbolKind::Variable;
        playerVar->typeInfo = "Player";
        table.AddGlobal(playerVar);

        size_t offset = code.find("hp; }"); // Finds the first one inside class
        size_t offset2 = code.find("hp;", offset + 5); // Finds the second one 'player.hp;'
        REQUIRE(offset2 != std::string::npos);

        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset2);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "hp");
        CHECK(sym->kind == SymbolKind::Variable);
    }

    TEST_CASE("ResolveAt namespace expression")
    {
        std::string code = "namespace Math { int PI = 3; } void Main() { int x = Math::PI; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("PI;");
        REQUIRE(offset != std::string::npos);

        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "PI");
        CHECK(sym->kind == SymbolKind::Variable);
    }

    TEST_CASE("ResolveAt local variables")
    {
        std::string code = "void Main() { int local_var = 5; local_var = 10; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 0); // func_declaration
        TSNode blockNode = ts_node_child(funcNode, 3); // statement_block
        SymbolCollector::TraverseLocals(blockNode, doc, table);

        size_t offset = code.find("local_var = 10;");
        REQUIRE(offset != std::string::npos);

        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "local_var");
        CHECK(sym->kind == SymbolKind::Variable);
    }

    TEST_CASE("ResolveAt member expression (T-20 verification)")
    {
        std::string code = "class Dog { void Bark() {} } void Main() { Dog d; d.Bark(); }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 1); // Main
        TSNode blockNode = ts_node_child(funcNode, 3); // statement_block
        SymbolCollector::TraverseLocals(blockNode, doc, table);

        size_t offset = code.find("Bark();");
        REQUIRE(offset != std::string::npos);

        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Bark");
        CHECK(sym->kind == SymbolKind::Function);
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Dog");
    }

    TEST_CASE("ResolveAt Combat::Fire (T-21 requirement)")
    {
        std::string code = "namespace Combat { void Fire() {} } void Main() { Combat::Fire(); }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("Fire();");
        REQUIRE(offset != std::string::npos);

        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        if (!sym)
        {
            printf("Dumping entire doc AST for debugging Combat::Fire:\n");
            // Just use the existing DumpAST from SymbolCollectorTests if we can, 
            // but we don't have it here. Let's write a small local lambda.
            auto dump = [&](auto& self, TSNode n, int depth) -> void {
                if (ts_node_is_null(n)) return;
                std::string indent(depth * 2, ' ');
                printf("%s%s (%s)\n", indent.c_str(), ts_node_type(n), std::string(doc.SourceAt(n)).c_str());
                for (uint32_t i=0; i<ts_node_child_count(n); i++) {
                    self(self, ts_node_child(n, i), depth + 1);
                }
            };
            dump(dump, doc.RootNode(), 0);
        }

        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Fire");
        CHECK(sym->kind == SymbolKind::Function);
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Combat");
    }

    TEST_CASE("ResolveAt empty space returns nullptr")
    {
        std::string code = "void Main() {       }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("       ") + 3;
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        CHECK(sym == nullptr);
    }
}
