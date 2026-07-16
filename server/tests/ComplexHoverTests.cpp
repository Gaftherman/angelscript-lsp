#include <doctest/doctest.h>
#include <iostream>
#include "helpers/TestFixtures.h"
#include "analysis/SymbolResolver.h"
#include "analysis/SymbolCollector.h"

using namespace analysis;

// rigorous test based on scratch.as
const char* SCRATCH_AS = R"(
shared class string {}

namespace Engine
{
    namespace Math
    {
        class Vector3
        {
            float x;
            float y;
            float z;

            Vector3() { x = 0; y = 0; z = 0; }
            Vector3(float ax, float ay, float az) { x = ax; y = ay; z = az; }

            float Length() { return x * x + y * y + z * z; }

            Vector3 opAdd(Vector3 other) { return Vector3(x + other.x, y + other.y, z + other.z); }
            Vector3 opMul(float s)       { return Vector3(x * s, y * s, z * s); }
        }
        
        float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        Vector3 VectorLerp(Vector3 a, Vector3 b, float t)
        {
            return Vector3(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t));
        }
    }

    namespace Physics
    {
        class RigidBody
        {
            Engine::Math::Vector3 position;
            Engine::Math::Vector3 velocity;
            float mass;

            void ApplyForce(Engine::Math::Vector3 force)
            {
                velocity = velocity + (force * (1.0f / mass));
            }
        }
    }
}

class Entity
{
    float hp;
    float maxHp;
    string name;

    Entity()
    {
        hp    = 100.0f;
    }

    void TakeDamage(float amount)
    {
        hp -= amount;
    }
}

class Spatial : Entity
{
    Engine::Math::Vector3 position;

    void MoveTo(Engine::Math::Vector3 target)
    {
        position = target;
    }
}

void Main()
{
    Engine::Physics::RigidBody body;
    body.mass = 75.0f;
    body.ApplyForce(Engine::Math::Vector3(0, -9.8f, 0));
    
    Spatial player;
    player.MoveTo(Engine::Math::Vector3(1.0f, 0.0f, 0.0f));
}
)";

TEST_SUITE("ComplexHover")
{
    TEST_CASE("CH1: Constructor resolution by argument count")
    {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);
        
        // Find "body.ApplyForce(Engine::Math::Vector3(0, -9.8f, 0));"
        size_t offset = code.find("Vector3(0, -9.8f, 0)");
        
        uint32_t line = 0;
        uint32_t col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        
        std::vector<const Symbol*> results;
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col, &results);
        
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Vector3");
        CHECK(sym->kind == SymbolKind::Constructor);
        CHECK(sym->params.size() == 3);
        CHECK(sym->signature == "Vector3(float ax, float ay, float az)");
    }
    
    TEST_CASE("CH2: Constructor resolution (no arguments)")
    {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);
        
        size_t offset = code.find("Vector3() { x = 0; y = 0; z = 0; }");
        
        uint32_t line = 0;
        uint32_t col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        
        std::vector<const Symbol*> results;
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col, &results);
        
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Vector3");
        CHECK(sym->kind == SymbolKind::Constructor);
        CHECK(sym->params.size() == 0);
        CHECK(sym->signature == "Vector3()");
    }
    
    TEST_CASE("CH3: Parameter resolution correctly resolves full typeInfo")
    {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);
        
        // find "ApplyForce(Engine::Math::Vector3 force)"
        size_t offset = code.find("force)", code.find("ApplyForce"));
        
        // Force the function's scope into the table by parsing locals
        auto traverseFuncs = [&](TSNode node, auto& self) -> void {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "method_declaration") {
                TSNode paramList = ts_node_child_by_field_name(node, "parameters", 10);
                if (!ts_node_is_null(paramList)) {
                    SymbolCollector::RegisterParamsAsLocals(paramList, doc, table);
                }
                TSNode body = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(body)) {
                    SymbolCollector::TraverseLocals(body, doc, table, nullptr);
                }
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) {
                self(ts_node_child(node, i), self);
            }
        };
        traverseFuncs(root, traverseFuncs);
        
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "force");
        CHECK(sym->kind == SymbolKind::Parameter);
        CHECK(sym->typeInfo == "Engine::Math::Vector3");
    }
    
    TEST_CASE("CH4: Intermediate namespace resolution in scoped_identifier")
    {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        size_t offset = code.find("Math::Vector3(0, -9.8f, 0)");
        
        uint32_t line = 0;
        uint32_t col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        
        std::vector<const Symbol*> results;
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col, &results);
        
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Math");
        CHECK(sym->kind == SymbolKind::Namespace);
    }
    
    TEST_CASE("CH5: Avoid parameter collisions using LocalByNameAt")
    {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);
        
        // Populate ALL locals from ALL functions, which would normally cause shadowing collisions
        auto traverseFuncs = [&](TSNode node, auto& self) -> void {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "method_declaration") {
                TSNode paramList = ts_node_child_by_field_name(node, "parameters", 10);
                if (!ts_node_is_null(paramList)) {
                    SymbolCollector::RegisterParamsAsLocals(paramList, doc, table);
                }
                TSNode body = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(body)) {
                    SymbolCollector::TraverseLocals(body, doc, table, nullptr);
                }
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) {
                self(ts_node_child(node, i), self);
            }
        };
        traverseFuncs(root, traverseFuncs);
        
        // Find "target" in "position = target" (inside MoveTo)
        // MoveTo is inside Spatial
        size_t offset = code.find("target;", code.find("position = target;"));
        
        // We get line and col for ResolveAt so FindLocalByNameAt can do its job
        uint32_t line = 0;
        uint32_t col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "target");
        CHECK(sym->kind == SymbolKind::Parameter);
        CHECK(sym->typeInfo == "Engine::Math::Vector3");
    }

    TEST_CASE("CH6: Resolving global function from inside a class in the same namespace") {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);

        size_t offset = code.find("Lerp(a.x");
        uint32_t line = 0;
        uint32_t col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Lerp");
        CHECK(sym->kind == SymbolKind::Function);
    }

    TEST_CASE("CH7: Method resolution on local variable with scoped_type") {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);

        auto traverseFuncs = [&](TSNode node, auto& self) -> void {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "constructor") {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode)) SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        size_t offset = code.find("ApplyForce", code.find("body.ApplyForce"));
        uint32_t line = 0;
        uint32_t col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "ApplyForce");
        CHECK(sym->kind == SymbolKind::Method);
    }

    TEST_CASE("CH8: Name collisions inside namespace") {
        std::string code = R"(
namespace Game
{
    class Player {}
    class FinalBoss {}

    // TEST: hover sobre 'Game::Player' -> "class Player"
    Player gamePlayer;

    // TEST: hover sobre 'Game::FinalBoss' -> "class FinalBoss"
    FinalBoss finalBoss;

    class Game 
    {
        void Start()
        {
            gamePlayer = Player();
            finalBoss  = FinalBoss();
        }
    }

    enum Game // Error de compilacion: enum con el mismo nombre que la clase
    {
        GS_INIT,
        GS_RUNNING,
        GS_PAUSED,
        GS_OVER
    }

    typedef int Game; // Error de compilacion: typedef con el mismo nombre que la clase y enum  

    funcdef void Game(); // Error de compilacion: funcdef con el mismo nombre que la clase, enum y typedef
}
        )";
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);

        auto traverseFuncs = [&](TSNode node, auto& self) -> void {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "constructor") {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode)) SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        auto getPos = [&](const std::string& target) -> std::pair<uint32_t, uint32_t> {
            size_t offset = code.find(target);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < offset; i++) {
                if (code[i] == '\n') { line++; col = 0; } else { col++; }
            }
            return {line, col};
        };

        // Test hover on 'Player gamePlayer;'
        {
            auto [line, col] = getPos("Player gamePlayer;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Player");
            CHECK(sym->kind == SymbolKind::Class);
        }

        // Test hover on 'FinalBoss finalBoss;'
        {
            auto [line, col] = getPos("FinalBoss finalBoss;");
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "FinalBoss");
            CHECK(sym->kind == SymbolKind::Class);
        }
        
        // Let's check hover on Game as the class declaration
        {
            auto [line, col] = getPos("class Game");
            col += 6; // pointing at 'Game'
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Game");
            CHECK(sym->kind == SymbolKind::Class);
        }
        
        // Let's check hover on Game as the enum declaration
        {
            auto [line, col] = getPos("enum Game");
            col += 5; // pointing at 'Game'
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Game");
            CHECK(sym->kind == SymbolKind::Enum);
        }
        
        // Let's check hover on Game as the typedef
        {
            auto [line, col] = getPos("typedef int Game;");
            col += 12; // pointing at 'Game'
            const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
            REQUIRE(sym != nullptr);
            CHECK(sym->name == "Game");
            CHECK(sym->kind == SymbolKind::Typedef);
        }
    }
}
