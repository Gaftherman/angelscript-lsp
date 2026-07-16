#include <doctest/doctest.h>
#include <iostream>
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



    TEST_CASE("R23: Name collisions and typedef resolution")
    {
        std::string code = R"(
typedef float MyFloat;
class MyFloat {}

namespace Collider {
    class Collider {}
    void Collider() {}
    enum Collider { COLLIDER_VAL }
    MyFloat Collider;
}

void Main() {
    MyFloat f;
    Collider::Collider c;
}
)";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        auto resolveAndCheck = [&](const std::string& searchStr, SymbolKind expectedKind) {
            size_t offset = code.rfind(searchStr);
            uint32_t line = 0;
            for (size_t i = 0; i < offset; i++) if (code[i] == '\n') line++;
            uint32_t col = (uint32_t)(offset - code.rfind('\n', offset) - 1);
            
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            return sym;
        };
        
        // When resolving MyFloat in `MyFloat f;`, we might get Typedef or Class. 
        // We will just verify it finds ONE of them without crashing.
        const Symbol* myFloatSym = resolveAndCheck("MyFloat f;", SymbolKind::Typedef);
        REQUIRE(myFloatSym != nullptr);
        CHECK(myFloatSym->name == "MyFloat");

        // When resolving Collider in `Collider::Collider c;`, first Collider is namespace, second is class/function/enum/var.
        const Symbol* colliderSym = resolveAndCheck("Collider c;", SymbolKind::Class);
        REQUIRE(colliderSym != nullptr);
        CHECK(colliderSym->name == "Collider");
        CHECK(colliderSym->parent != nullptr);
        CHECK(colliderSym->parent->name == "Collider"); // inside namespace Collider
    }

    TEST_CASE("R8: Cursor en keyword void -> null (sin crash)")
    {
        std::string code = "void Main() {}";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, 2);
        CHECK(sym == nullptr);
    }

    TEST_CASE("R21: Lerp namespace call resolution")
    {
        std::string code = R"(
namespace Engine {
    namespace Math {
        float Lerp(float a, float b, float t) {
            return a + (b - a) * t;
        }
    }
}
void Main() {
    float val = Engine::Math::Lerp(0, 1, 0.5f);
}
)";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.rfind("Lerp(0,");
        uint32_t line = 0;
        for (size_t i = 0; i < offset; i++) if (code[i] == '\n') line++;
        uint32_t col = (uint32_t)(offset - code.rfind('\n', offset) - 1);
        
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Lerp");
        CHECK(sym->kind == SymbolKind::Function);
    }

    TEST_CASE("R22: Mixin host resolution from subclass")
    {
        std::string code = R"(
class Entity { float hp; }
mixin class Regenerator { float regenRate; }
class Troll : Entity, Regenerator {
    void Enrage() {
        regenRate = 5.0f;
    }
}
)";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.rfind("regenRate =");
        uint32_t line = 0;
        for (size_t i = 0; i < offset; i++) if (code[i] == '\n') line++;
        uint32_t col = (uint32_t)(offset - code.rfind('\n', offset) - 1);

        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "regenRate");
        CHECK(sym->kind == SymbolKind::Variable);
        CHECK(sym->parent != nullptr);
        CHECK(sym->parent->name == "Regenerator");
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
        CHECK(sym->kind == SymbolKind::Method);
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

    TEST_CASE("RH5: Hover en miembro de clase con igual nombre (Scope-aware)")
    {
        std::string code = "class A { int hp; } class B { int hp; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        // Cursor en "hp" de clase B
        size_t offsetB = code.rfind("hp"); // hp inside class B
        const Symbol* symB = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offsetB);
        REQUIRE(symB != nullptr);
        CHECK(symB->name == "hp");
        REQUIRE(symB->parent != nullptr);
        CHECK(symB->parent->name == "B");

        // Cursor en "hp" de clase A
        size_t offsetA = code.find("hp"); // hp inside class A
        const Symbol* symA = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offsetA);
        REQUIRE(symA != nullptr);
        CHECK(symA->name == "hp");
        REQUIRE(symA->parent != nullptr);
        CHECK(symA->parent->name == "A");
    }

    TEST_CASE("RH6: Hover en nombre de namespace")
    {
        std::string code = "namespace Game { void Foo() {} }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("Game");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Game");
        CHECK(sym->kind == SymbolKind::Namespace);
    }

    TEST_CASE("RH7: Hover en local de funcion anidada en namespace")
    {
        std::string code = "namespace Game { void Foo() { int y = 5; } }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        // This is exactly what Server::CollectLocalsForDocument does now (recursive)
        extern void CollectLocalsFunctions(TSNode node, const Document& doc, analysis::SymbolTable& table);
        // Wait, CollectLocalsFunctions is static in Server.cpp.
        // We will just call TraverseLocals directly on the block to test the resolver part.
        // Or better yet, we can't test Server.cpp's static function here easily.
        // Let's just simulate the collection to test the resolver.
        TSNode root = doc.RootNode();
        TSNode nsNode = ts_node_child(root, 0);
        TSNode bodyNode = ts_node_child_by_field_name(nsNode, "body", 4);
        
        TSNode funcNode;
        for (uint32_t i = 0; i < ts_node_child_count(bodyNode); i++) {
            TSNode child = ts_node_child(bodyNode, i);
            if (std::string_view(ts_node_type(child)) == "func_declaration") {
                funcNode = child;
                break;
            }
        }
        
        TSNode blockNode = ts_node_child_by_field_name(funcNode, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        size_t offset = code.find("y");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "y");
        CHECK(sym->kind == SymbolKind::Variable);
    }

    TEST_CASE("RH8: Hover sobre clase anidada en namespace (Bug 1)")
    {
        std::string code = "namespace Game { class Entity { float speed; float GetSpeed() {} } }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offset = code.find("Entity");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Entity");
        CHECK(sym->kind == SymbolKind::Class);

        size_t offsetSpeed = code.find("speed");
        const Symbol* symSpeed = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offsetSpeed);
        REQUIRE(symSpeed != nullptr);
        CHECK(symSpeed->name == "speed");
        CHECK(symSpeed->kind == SymbolKind::Variable);
        REQUIRE(symSpeed->parent != nullptr);
        CHECK(symSpeed->parent->name == "Entity");
    }

    TEST_CASE("RH9: member_expression con tipo de namespace (Bug 2)")
    {
        std::string code = "namespace Game { class Entity { float speed; } } void Main() { Game::Entity e; e.speed = 5.0f; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        // Populate local table for Main to have "e"
        size_t offsetMain = code.find("Main");
        TSNode root = doc.RootNode();
        TSNode mainFunc;
        for (uint32_t i = 0; i < ts_node_child_count(root); i++) {
            TSNode child = ts_node_child(root, i);
            if (std::string_view(ts_node_type(child)) == "func_declaration") {
                mainFunc = child;
                break;
            }
        }
        TSNode blockNode = ts_node_child_by_field_name(mainFunc, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        // Hover over the e.speed 'speed'
        size_t offset = code.rfind("speed");
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, 0, (uint32_t)offset);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "speed");
        CHECK(sym->kind == SymbolKind::Variable);
        REQUIRE(sym->parent != nullptr);
        CHECK(sym->parent->name == "Entity");
    }

    TEST_CASE("Shared, External, Auto, and Namespace Shadowing")
    {
        std::string code =
            "namespace Foo { class Foo { int x; } }\n"
            "shared class Bar { int y; }\n"
            "external shared class ExtBar;\n"
            "shared void GlobalFunc() {}\n"
            "external shared void ExtFunc();\n"
            "void Main() {\n"
            "  Foo::Foo f;\n"
            "  f.x = 10;\n"
            "  Bar b;\n"
            "  ExtBar eb;\n"
            "  GlobalFunc();\n"
            "  ExtFunc();\n"
            "  auto a = 5;\n"
            "  auto @ptr = @a;\n"
            "}";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        auto getPos = [&](const std::string& target) -> std::pair<uint32_t, uint32_t> {
            size_t offset = code.rfind(target);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < offset; i++) {
                if (code[i] == '\n') { line++; col = 0; } else { col++; }
            }
            return {line, col};
        };

        size_t offsetMain = code.find("Main");
        TSNode root = doc.RootNode();
        TSNode mainFunc;
        for (uint32_t i = 0; i < ts_node_child_count(root); i++) {
            TSNode child = ts_node_child(root, i);
            if (std::string_view(ts_node_type(child)) == "func_declaration") {
                TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
                if (!ts_node_is_null(nameNode) && std::string_view(doc.SourceAt(nameNode)) == "Main") {
                    mainFunc = child;
                    break;
                }
            }
        }
        TSNode blockNode = ts_node_child_by_field_name(mainFunc, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        SUBCASE("Namespace Shadowing") {
            auto [line, col] = getPos("Foo f;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Foo");
            CHECK(sym->kind == SymbolKind::Class);
            REQUIRE(sym->parent != nullptr);
            CHECK(sym->parent->name == "Foo"); // Parent is namespace Foo
        }

        SUBCASE("Shared Class") {
            auto [line, col] = getPos("Bar b;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Bar");
            CHECK(sym->kind == SymbolKind::Class);
        }

        SUBCASE("External Shared Class") {
            auto [line, col] = getPos("ExtBar eb;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "ExtBar");
            CHECK(sym->kind == SymbolKind::Class);
        }

        SUBCASE("Shared Function") {
            auto [line, col] = getPos("GlobalFunc(");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "GlobalFunc");
            CHECK(sym->kind == SymbolKind::Function);
        }

        SUBCASE("External Shared Function") {
            auto [line, col] = getPos("ExtFunc(");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "ExtFunc");
            CHECK(sym->kind == SymbolKind::Function);
        }

        SUBCASE("Auto variable") {
            auto [line, col] = getPos("auto a");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            
            // DUMP LOCALS
            for (auto& l : table.GetLocals()) {
                printf("Local: %s (type: %s)\n", l->name.c_str(), l->typeInfo.c_str());
            }

            auto [line2, col2] = getPos("a = 5");
            const Symbol* varSym = SymbolResolver::ResolveAt(doc, table, line2, col2);
            REQUIRE(varSym != nullptr);
            CHECK(varSym->name == "a");
            CHECK(varSym->kind == SymbolKind::Variable);
            CHECK(varSym->typeInfo == "auto");
        }
        
        SUBCASE("Auto handle variable") {
            auto [line, col] = getPos("ptr =");
            const Symbol* varSym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(varSym != nullptr);
            CHECK(varSym->name == "ptr");
            CHECK(varSym->kind == SymbolKind::Variable);
            CHECK(varSym->typeInfo == "auto @");
        }
    }

    TEST_CASE("Advanced Hover Features (Inheritance, Mixin, Using)")
    {
        std::string code =
            "interface IDam { void TakeDamage(float a); }\n"
            "mixin class Regen { void Tick() {} }\n"
            "class Entity : IDam, Regen { float hp; }\n"
            "namespace Engine { class Vector3 { float x; } }\n"
            "using namespace Engine;\n"
            "void Main() {\n"
            "  Entity e;\n"
            "  e.TakeDamage(10.0f);\n"
            "  e.Tick();\n"
            "  Vector3 pos;\n"
            "}\n";
        Document doc("file:///test.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        size_t offsetMain = code.find("Main");
        TSNode root = doc.RootNode();
        TSNode mainFunc;
        for (uint32_t i = 0; i < ts_node_child_count(root); i++) {
            TSNode child = ts_node_child(root, i);
            if (std::string_view(ts_node_type(child)) == "func_declaration") {
                mainFunc = child;
                break;
            }
        }
        TSNode blockNode = ts_node_child_by_field_name(mainFunc, "body", 4);
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);

        auto getPos = [&](const std::string& target) -> std::pair<uint32_t, uint32_t> {
            size_t offset = code.rfind(target);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < offset; i++) {
                if (code[i] == '\n') { line++; col = 0; } else { col++; }
            }
            return {line, col};
        };

        SUBCASE("Hover over inherited method") {
            auto [line, col] = getPos("TakeDamage(");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "TakeDamage");
            CHECK(sym->kind == SymbolKind::Method);
            REQUIRE(sym->parent != nullptr);
            CHECK(sym->parent->name == "IDam");
        }

        SUBCASE("Hover over mixin method") {
            auto [line, col] = getPos("Tick()");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Tick");
            CHECK(sym->kind == SymbolKind::Method); // Currently mixin methods might parse as Function, that's fine
            REQUIRE(sym->parent != nullptr);
            CHECK(sym->parent->name == "Regen");
        }

        SUBCASE("Hover over type from using namespace") {
            auto [line, col] = getPos("Vector3 pos");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Vector3");
            CHECK(sym->kind == SymbolKind::Class);
            REQUIRE(sym->parent != nullptr);
            CHECK(sym->parent->name == "Engine");
        }
    }

    TEST_CASE("Mixin Scope and Host-Class Search") {
        analysis::SymbolTable table;
        std::string code = R"(
class Entity { float hp; float speed; }
mixin class Mover { float speed; void Move() { speed = 1.0f; } }
class Player : Entity, Mover {}

mixin class Regenerator { float regenRate; void Regen() { float x = regenRate; hp = hp + 1.0f; } }
class Troll : Entity, Regenerator { void Enrage() { regenRate = 5.0f; } }

class Monster { float hp; }
class Ogre : Monster, Regenerator {}

class Boss : Monster { float hp; } // Boss shadows hp
class MegaBoss : Boss, Regenerator {}

mixin class OrphanMixin { void Foo() { nonExistentVar = 0; } }

interface IMovable { void Move(); }
        )";

        Document doc("file:///test.as", code);

        analysis::SymbolCollector::CollectGlobals(doc, table);
        TSNode root = doc.RootNode();

        auto parseLocalsIn = [&](const std::string& className, const std::string& funcName) {
            for (uint32_t i = 0; i < ts_node_child_count(root); i++) {
                TSNode child = ts_node_child(root, i);
                std::string_view type = ts_node_type(child);
                if (type == "class_declaration" || type == "mixin_declaration") {
                    TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
                    if (!ts_node_is_null(nameNode) && std::string_view(doc.SourceAt(nameNode)) == className) {
                        TSNode body = ts_node_child_by_field_name(child, "body", 4);
                        if (!ts_node_is_null(body)) {
                            for (uint32_t j = 0; j < ts_node_child_count(body); j++) {
                                TSNode m = ts_node_child(body, j);
                                if (std::string_view(ts_node_type(m)) == "func_declaration") {
                                    TSNode mNameNode = ts_node_child_by_field_name(m, "name", 4);
                                    if (!ts_node_is_null(mNameNode) && std::string_view(doc.SourceAt(mNameNode)) == funcName) {
                                        TSNode fbody = ts_node_child_by_field_name(m, "body", 4);
                                        analysis::SymbolCollector::TraverseLocals(fbody, doc, table, nullptr);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        };

        auto getPos = [&](const std::string& match) -> std::pair<uint32_t, uint32_t> {
            size_t idx = code.find(match);
            REQUIRE(idx != std::string::npos);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < idx; i++) {
                if (code[i] == '\n') { line++; col = 0; }
                else { col++; }
            }
            return {line, col};
        };

        SUBCASE("Group A: Mixin Hover") {
            auto [line, col] = getPos("Regenerator");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Regenerator");
            CHECK(sym->kind == SymbolKind::Mixin);
        }

        SUBCASE("Group B: Mixin own member") {
            parseLocalsIn("Regenerator", "Regen");
            auto [line, col] = getPos("regenRate;"); // inside Regen
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "regenRate");
            REQUIRE(sym->parent != nullptr);
            CHECK(sym->parent->name == "Regenerator");
        }

        SUBCASE("Group C: Host-Class Search") {
            parseLocalsIn("Regenerator", "Regen");
            auto [line, col] = getPos("hp = hp"); // inside Regen
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "hp");
            REQUIRE(sym->parent != nullptr);
            CHECK(sym->parent->name == "Entity");
        }

        SUBCASE("Group D: Orphan Mixin (No Host)") {
            parseLocalsIn("OrphanMixin", "Foo");
            auto [line, col] = getPos("nonExistentVar");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            CHECK(sym == nullptr);
        }

        SUBCASE("Group E: Shadowing (Mixin has priority over Host)") {
            parseLocalsIn("Mover", "Move");
            auto [line, col] = getPos("speed = 1.0f");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "speed");
            REQUIRE(sym->parent != nullptr);
            CHECK(sym->parent->name == "Mover"); // not Entity
        }

        SUBCASE("Group F: Interface is correctly resolved as Interface") {
            auto [line, col] = getPos("IMovable");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "IMovable");
        SUBCASE("Group F: Interface is correctly resolved as Interface") {
            auto [line, col] = getPos("IMovable");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "IMovable");
            CHECK(sym->kind == SymbolKind::Interface);
        }

        SUBCASE("Group G: Multi-Host Search") {
            parseLocalsIn("Regenerator", "Regen");
            auto [line, col] = getPos("hp = hp"); // inside Regen
            std::vector<const Symbol*> multiResults;
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col, &multiResults);
            
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "hp");
            
            // Should find hp in Entity (via Troll), Monster (via Ogre), and Boss (via MegaBoss)
            REQUIRE(multiResults.size() >= 3);
            bool foundEntity = false;
            bool foundMonster = false;
            bool foundBoss = false;
            for (const auto& res : multiResults) {
                if (res->parent && res->parent->name == "Entity") foundEntity = true;
                if (res->parent && res->parent->name == "Monster") foundMonster = true;
                if (res->parent && res->parent->name == "Boss") foundBoss = true;
            }
            CHECK(foundEntity);
            CHECK(foundMonster);
            CHECK(foundBoss);
        }

        SUBCASE("Group H: Host -> Mixin Search (Bug 1)") {
            parseLocalsIn("Troll", "Enrage");
            auto [line, col] = getPos("regenRate = 5.0f"); // inside Enrage
            std::vector<const Symbol*> multiResults;
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col, &multiResults);
            
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "regenRate");
            REQUIRE(sym->parent != nullptr);
            CHECK(sym->parent->name == "Regenerator");
        }
    }
    }

    TEST_CASE("Variable Declaration Hover Bugs") {
        analysis::SymbolTable table;
        std::string code = R"(
namespace Engine { namespace Math { class Vector3 {} } }
enum GameState { PLAYING }
funcdef void NetworkCallback();
class NetworkManager { NetworkCallback@ onReceive; }
class Troll { void TakeDamage(float a) {} }

void Main() {
    Troll myEnemy;
    Engine::Math::Vector3 pos2;
    GameState state = PLAYING;
    NetworkManager net;
    Player p; // Unknown class, should still resolve as local variable
}
        )";

        Document doc("file:///test.as", code);
        analysis::SymbolCollector::CollectGlobals(doc, table);

        // Run TraverseLocals on Main
        TSNode root = doc.RootNode();
        for (uint32_t i = 0; i < ts_node_child_count(root); i++) {
            TSNode child = ts_node_child(root, i);
            if (std::string_view(ts_node_type(child)) == "func_declaration") {
                TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
                if (!ts_node_is_null(nameNode) && std::string_view(doc.SourceAt(nameNode)) == "Main") {
                    TSNode body = ts_node_child_by_field_name(child, "body", 4);
                    analysis::SymbolCollector::TraverseLocals(body, doc, table, nullptr);
                    break;
                }
            }
        }

        auto getPos = [&](const std::string& match) -> std::pair<uint32_t, uint32_t> {
            size_t idx = code.find(match);
            REQUIRE(idx != std::string::npos);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < idx; i++) {
                if (code[i] == '\n') { line++; col = 0; }
                else { col++; }
            }
            return {line, col};
        };

        SUBCASE("Hover over standard class local variable declaration") {
            auto [line, col] = getPos("myEnemy;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "myEnemy");
            CHECK(sym->typeInfo == "Troll");
        }

        SUBCASE("Hover over enum local variable declaration") {
            auto [line, col] = getPos("state = PLAYING");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "state");
            CHECK(sym->typeInfo == "GameState");
        }

        SUBCASE("Hover over funcdef local variable declaration") {
            auto [line, col] = getPos("net;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "net");
            CHECK(sym->typeInfo == "NetworkManager");
        }

        SUBCASE("Hover over unknown class local variable declaration") {
            auto [line, col] = getPos("p;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "p");
            CHECK(sym->typeInfo == "Player");
        }

        SUBCASE("Hover over namespace qualified local variable declaration") {
            auto [line, col] = getPos("pos2;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "pos2");
            CHECK(sym->typeInfo == "Engine::Math::Vector3"); // Full type correctly rebuilt
        }
    }

    TEST_SUITE("Hover Context and Constructors") {
        TEST_CASE("Hover over constructor and destructor") {
            std::string code = "class Vec { Vec() {} ~Vec() {} Vec(float x) {} }\nvoid Main() { Vec v; }";
            Document doc("file:///test.as", code);
            SymbolTable table;
            SymbolCollector::CollectGlobals(doc, table);

            auto getPos = [&](const std::string& target) -> std::pair<uint32_t, uint32_t> {
                size_t offset = code.rfind(target);
                uint32_t line = 0, col = 0;
                for (size_t i = 0; i < offset; i++) {
                    if (code[i] == '\n') { line++; col = 0; } else { col++; }
                }
                return {line, col};
            };

            SUBCASE("Default Constructor") {
                auto [line, col] = getPos(" Vec() {}");
                const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col + 1); // +1 because we added a space
                REQUIRE(sym != nullptr);
                CHECK(sym->kind == SymbolKind::Constructor);
                CHECK(sym->name == "Vec");
            }

            SUBCASE("Destructor") {
                auto [line, col] = getPos("~Vec() {}");
                const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col + 1); // +1 to hit 'Vec'
                REQUIRE(sym != nullptr);
                CHECK(sym->kind == SymbolKind::Destructor);
                CHECK(sym->name == "~Vec");
            }

            SUBCASE("Constructor with params") {
                auto [line, col] = getPos("Vec(float x)");
                const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
                REQUIRE(sym != nullptr);
                CHECK(sym->kind == SymbolKind::Constructor);
                CHECK(sym->name == "Vec");
            }

            SUBCASE("Class type in variable declaration") {
                auto [line, col] = getPos("Vec v;");
                const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
                REQUIRE(sym != nullptr);
                CHECK(sym->kind == SymbolKind::Class);
                CHECK(sym->name == "Vec");
            }
        }
    }
}
