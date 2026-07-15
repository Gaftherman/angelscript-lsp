#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"
#include "analysis/SymbolResolver.h"
#include "analysis/SymbolCollector.h"

using namespace analysis;

TEST_SUITE("SymbolResolver")
{
    TEST_CASE("R1: Resolver funcion global en punto de uso")
    {
        Document doc("file:///test.as", "void DealDamage(int x) {} void Main() { DealDamage(5); }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        size_t offset = std::string("void DealDamage(int x) {} void Main() { ").length();
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "DealDamage");
        CHECK(sym->kind == SymbolKind::Function);
        CHECK(sym->selectionRange.start.character == 5);
        CHECK(sym->selectionRange.start.line == 0);
    }

    TEST_CASE("R2: Resolver variable global")
    {
        Document doc("file:///test.as", "int g_score = 0; void Main() { g_score = 10; }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        size_t offset = std::string("int g_score = 0; void Main() { ").length();
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "g_score");
        CHECK(sym->kind == SymbolKind::Variable);
    }

    TEST_CASE("R3: Resolver variable local")
    {
        std::string code = "void Main() { int localHP = 100; localHP -= 10; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 0); 
        TSNode blockNode = ts_node_child_by_field_name(funcNode, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        size_t offset = code.find("localHP -=");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "localHP");
        CHECK(sym->kind == SymbolKind::Variable);
        CHECK(sym->selectionRange.start.character == 18); // Actually it's 18 because "void Main() { int localHP"
    }

    TEST_CASE("R4: Resolver miembro de clase (member_expression con handle @)")
    {
        std::string code = "class Dog { void Bark() {} } void Main() { Dog@ d; d.Bark(); }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 1);
        TSNode blockNode = ts_node_child_by_field_name(funcNode, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        size_t offset = code.find("Bark();");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Bark");
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Dog");
    }

    TEST_CASE("R5: Resolver campo de clase")
    {
        std::string code = "class Actor { int hp; } void Main() { Actor a; a.hp = 50; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 1);
        TSNode blockNode = ts_node_child_by_field_name(funcNode, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        size_t offset = code.find("hp = 50;");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "hp");
        CHECK(sym->kind == SymbolKind::Variable);
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Actor");
    }

    TEST_CASE("R6: Resolver simbolo en namespace (::)")
    {
        std::string code = "namespace Combat { void Fire() {} } void Main() { Combat::Fire(); }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("Fire();");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Fire");
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Combat");
    }

    TEST_CASE("R7: Resolver enum member")
    {
        std::string code = "enum State { IDLE, RUN } void Main() { State s = IDLE; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.rfind("IDLE;");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        if (!sym) {
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
        CHECK(sym->name == "IDLE");
        CHECK(sym->kind == SymbolKind::EnumMember);
    }

    TEST_CASE("R8: Cursor en keyword void -> null (sin crash)")
    {
        std::string code = "void Main() {}";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, 0);
        CHECK(sym == nullptr);
    }

    TEST_CASE("R9: Cursor en espacio en blanco -> null")
    {
        std::string code = "void Main() {   }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("   ") + 1;
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        CHECK(sym == nullptr);
    }

    TEST_CASE("R10: Cursor en literal numero -> null")
    {
        std::string code = "void Main() { int x = 42; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 0);
        TSNode blockNode = ts_node_child_by_field_name(funcNode, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        size_t offset = code.find("42;");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        CHECK(sym == nullptr);
    }

    TEST_CASE("R11: Resolver funcion con handle como parametro")
    {
        std::string code = "void Spawn(Player@ target) {} void Main() { Spawn(null); }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("Spawn(null);");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Spawn");
        REQUIRE(sym->params.size() == 1);
        CHECK(sym->params[0].typeName == "Player@");
    }

    TEST_CASE("R12: Resolver funcion en namespace anidado Engine::Math")
    {
        std::string code = "namespace Engine::Math { float Lerp(float a, float b, float t) {} } void Main() { Engine::Math::Lerp(0f, 1f, 0.5f); }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("Lerp(0f");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Lerp");
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Math");
        REQUIRE(sym->parent->parent != nullptr);
        CHECK(sym->parent->parent->name == "Engine");
    }

    TEST_CASE("RH1: Hover en miembro de clase (Deep Search)")
    {
        std::string code = "class Actor { float m_speed; float GetSpeed() {} }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("m_speed");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "m_speed");
        CHECK(sym->kind == SymbolKind::Variable);
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Actor");
    }

    TEST_CASE("RH2: Hover en metodo de clase (Deep Search)")
    {
        std::string code = "class Actor { float m_speed; float GetSpeed() {} }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("GetSpeed");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "GetSpeed");
        CHECK(sym->kind == SymbolKind::Function);
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Actor");
    }

    TEST_CASE("RH3: Hover en parametro de funcion en declaracion")
    {
        std::string code = "void DealDamage(Player@ target, int &out actualDamage) {}";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("target");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "target");
        CHECK(sym->kind == SymbolKind::Parameter);
        CHECK(sym->typeInfo == "Player@");
    }

    TEST_CASE("RH4: Hover en parametro en uso dentro del body")
    {
        std::string code = "void Foo(int x) { int y = x + 1; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 0); 
        TSNode blockNode = ts_node_child_by_field_name(funcNode, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        size_t offset = code.rfind("x"); // The 'x' in 'x + 1'
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "x");
        CHECK(sym->kind == SymbolKind::Parameter);
    }
}
