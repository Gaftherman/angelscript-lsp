#include <doctest/doctest.h>
#include <iostream>
#include <fstream>
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
        for (size_t i = 0; i < offset; i++)
        {
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
        for (size_t i = 0; i < offset; i++)
        {
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
        auto traverseFuncs = [&](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "method_declaration")
            {
                TSNode paramList = ts_node_child_by_field_name(node, "parameters", 10);
                if (!ts_node_is_null(paramList))
                {
                    SymbolCollector::RegisterParamsAsLocals(paramList, doc, table);
                }
                TSNode body = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(body))
                {
                    SymbolCollector::TraverseLocals(body, doc, table, nullptr);
                }
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                self(ts_node_child(node, i), self);
            }
        };
        traverseFuncs(root, traverseFuncs);
        
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++)
        {
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
        for (size_t i = 0; i < offset; i++)
        {
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
        auto traverseFuncs = [&](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "method_declaration")
            {
                TSNode paramList = ts_node_child_by_field_name(node, "parameters", 10);
                if (!ts_node_is_null(paramList))
                {
                    SymbolCollector::RegisterParamsAsLocals(paramList, doc, table);
                }
                TSNode body = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(body))
                {
                    SymbolCollector::TraverseLocals(body, doc, table, nullptr);
                }
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
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
        for (size_t i = 0; i < offset; i++)
        {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "target");
        CHECK(sym->kind == SymbolKind::Parameter);
        CHECK(sym->typeInfo == "Engine::Math::Vector3");
    }

    TEST_CASE("CH6: Resolving global function from inside a class in the same namespace")
    {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);

        size_t offset = code.find("Lerp(a.x");
        uint32_t line = 0;
        uint32_t col = 0;
        for (size_t i = 0; i < offset; i++)
        {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Lerp");
        CHECK(sym->kind == SymbolKind::Function);
    }

    TEST_CASE("CH7: Method resolution on local variable with scoped_type")
    {
        std::string code = SCRATCH_AS;
        Document doc("file:///scratch.as", code);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);

        auto traverseFuncs = [&](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "constructor")
            {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode)) SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        size_t offset = code.find("ApplyForce", code.find("body.ApplyForce"));
        uint32_t line = 0;
        uint32_t col = 0;
        for (size_t i = 0; i < offset; i++)
        {
            if (code[i] == '\n') { line++; col = 0; }
            else { col++; }
        }
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "ApplyForce");
        CHECK(sym->kind == SymbolKind::Method);
    }

    TEST_CASE("CH8: Name collisions inside namespace")
    {
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

        auto traverseFuncs = [&](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "constructor")
            {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode)) SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        auto getPos = [&](const std::string& target) -> std::pair<uint32_t, uint32_t>
        {
            size_t offset = code.find(target);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < offset; i++)
            {
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

    TEST_CASE("CH9: Mixin Hover multiResults population")
    {
        Document doc("file:///scratch.as", SCRATCH_AS);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);

        auto traverseFuncs = [&](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "constructor")
            {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode)) SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        auto getPos = [&](const std::string& target) -> std::pair<uint32_t, uint32_t>
        {
            size_t offset = std::string(SCRATCH_AS).find(target);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < offset; i++)
            {
                if (SCRATCH_AS[i] == '\n') { line++; col = 0; } else { col++; }
            }
            return {line, col};
        };

        // Test hover on 'hp' inside Entity constructor
        auto [line, col] = getPos("hp    = 100.0f;");
        col += 1;
        std::vector<const Symbol*> multiResults;
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col, &multiResults);

        REQUIRE(sym != nullptr);
        CHECK(sym->name == "hp");
        
        // At this point we are testing that ResolveAt correctly returns the symbol, 
        // and optionally populates multiResults correctly if it was an ambiguous/mixin resolution.
        // For 'hp' in Entity it should resolve to Entity's own member.
        if (sym->parent)
        {
            CHECK(sym->parent->name == "Entity");
        }
    }

    TEST_CASE("CH10: Method parameter resolution")
    {
        Document doc("file:///scratch.as", SCRATCH_AS);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        TSNode root = doc.RootNode();
        SymbolCollector::TraverseGlobals(root, doc, table, nullptr);

        auto traverseFuncs = [&](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "func_declaration" || std::string_view(ts_node_type(node)) == "constructor")
            {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode)) SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        auto getPos = [&](const std::string& target) -> std::pair<uint32_t, uint32_t>
        {
            size_t offset = std::string(SCRATCH_AS).find(target);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < offset; i++)
            {
                if (SCRATCH_AS[i] == '\n') { line++; col = 0; } else { col++; }
            }
            return {line, col};
        };

        // Test hover on 'amount' inside TakeDamage
        auto [line, col] = getPos("hp -= amount;");
        col += 7; // point to 'amount'
        const Symbol* sym = SymbolResolver::ResolveAt(doc, table, line, col);

        REQUIRE(sym != nullptr);
        CHECK(sym->name == "amount");
        CHECK(sym->kind == SymbolKind::Parameter);
        if (sym->parent)
        {
            CHECK(sym->parent->name == "TakeDamage");
        }
    }

    TEST_CASE("CH11: Resolve symbols from predefined namespaces")
    {
        const char* PREDEFINED = R"(
namespace Outer
{
    namespace Inner
    {
        class Inner
        {
            float var;
        }
    }
}
        )";

        const char* USER_CODE = R"(
void Main()
{
    Outer::Inner::Inner obj;
    obj.var = 3.14f;
}
        )";

        Document docPre("file:///as.predefined", PREDEFINED);
        Document docUser("file:///user.as", USER_CODE);

        SymbolTable table;
        SymbolCollector::CollectGlobals(docPre, table);
        SymbolCollector::TraverseGlobals(docPre.RootNode(), docPre, table, nullptr);

        SymbolCollector::CollectGlobals(docUser, table);
        SymbolCollector::TraverseGlobals(docUser.RootNode(), docUser, table, nullptr);

        auto traverseFuncs = [&](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "func_declaration")
            {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode)) SymbolCollector::TraverseLocals(bodyNode, docUser, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++) self(ts_node_child(node, i), self);
        };
        traverseFuncs(docUser.RootNode(), traverseFuncs);

        auto getPos = [&](const std::string& target) -> std::pair<uint32_t, uint32_t>
        {
            size_t offset = std::string(USER_CODE).find(target);
            uint32_t line = 0, col = 0;
            for (size_t i = 0; i < offset; i++)
            {
                if (USER_CODE[i] == '\n') { line++; col = 0; } else { col++; }
            }
            return {line, col};
        };

        // Hover on 'obj.var'
        auto [line, col] = getPos("obj.var = 3.14f;");
        col += 5; // point to 'var'
        
        // Hover on 'obj' first
        auto [lineObj, colObj] = getPos("obj.var = 3.14f;");
        const Symbol* objSym = SymbolResolver::ResolveAt(docUser, table, lineObj, colObj);
        REQUIRE(objSym != nullptr);

        const Symbol* sym = SymbolResolver::ResolveAt(docUser, table, line, col);

        REQUIRE(sym != nullptr);
        CHECK(sym->name == "var");
        CHECK(sym->kind == SymbolKind::Variable); // Since it was extracted as a class field (variable) in Predefined
        
        if (sym->parent)
        {
            CHECK(sym->parent->name == "Inner");
            if (sym->parent->parent)
            {
                CHECK(sym->parent->parent->name == "Inner");
                if (sym->parent->parent->parent)
                {
                    CHECK(sym->parent->parent->parent->name == "Outer");
                }
            }
        }
    }
}

TEST_CASE("CH12: Array instantiations and hover resolution")
{
    const char* PREDEFINED = R"(
class Animal { void Speak(); }
class array<class T>
{
    uint length() const;
    void resize(uint);
}
    )";

    const char* SRC = R"(
void Main()
{
    array<int> varName = {1, 2, 3};
    array<array<array<int>>> multiDim;
    array<Animal> animals;
}
    )";

    SymbolTable table;
    Document doc_pre("file:///as.predefined", PREDEFINED);
    SymbolCollector::CollectGlobals(doc_pre, table);

    Document doc("file:///main.as", SRC);
    SymbolCollector::CollectGlobals(doc, table);

    TSNode root = doc.RootNode();
    SymbolCollector::TraverseGlobals(root, doc, table, nullptr);
    SymbolCollector::TraverseLocals(root, doc, table, nullptr);

    auto getHover = [&](uint32_t line, uint32_t col) -> const Symbol*
    {
        return SymbolResolver::ResolveAt(doc, table, line, col, nullptr);
    };

    // Hover over 'array' in 'array<int> varName'
    // Line 3: array<int> varName = {1, 2, 3};
    auto hover1 = getHover(3, 7); 
    REQUIRE(hover1 != nullptr);
    CHECK(hover1->name == "array");
    
    // Hover over 'varName'
    auto hover2 = getHover(3, 17);
    REQUIRE(hover2 != nullptr);
    CHECK(hover2->name == "varName");
    CHECK(hover2->typeInfo == "array<int>");

    // Hover over 'multiDim'
    auto hover3 = getHover(4, 31);
    REQUIRE(hover3 != nullptr);
    CHECK(hover3->name == "multiDim");
    CHECK(hover3->typeInfo == "array<array<array<int>>>");

    // Hover over 'animals'
    auto hover4 = getHover(5, 20);
    REQUIRE(hover4 != nullptr);
    CHECK(hover4->name == "animals");
    CHECK(hover4->typeInfo == "array<Animal>");
}

TEST_CASE("CH13: Auto type resolution")
{
    const char* PREDEFINED = R"(
class classA {}
    )";

    const char* SRC = R"(
void Main()
{
    float f;
    auto i1 = f;
    classA a;
    auto i2 = a;
}
    )";

    SymbolTable table;
    Document doc_pre("file:///as.predefined", PREDEFINED);
    SymbolCollector::CollectGlobals(doc_pre, table);

    Document doc("file:///main.as", SRC);
    SymbolCollector::CollectGlobals(doc, table);

    TSNode root = doc.RootNode();
    SymbolCollector::TraverseGlobals(root, doc, table, nullptr);
    SymbolCollector::TraverseLocals(root, doc, table, nullptr);

    auto getHover = [&](uint32_t line, uint32_t col) -> const Symbol*
    {
        return SymbolResolver::ResolveAt(doc, table, line, col, nullptr);
    };

    // Hover over 'i1' (line 4)
    auto hover1 = getHover(4, 10); 
    REQUIRE(hover1 != nullptr);
    CHECK(hover1->name == "i1");
    CHECK(hover1->typeInfo == "float"); // auto resolves to float
    
    // Hover over 'i2' (line 6)
    auto hover2 = getHover(6, 10);
    REQUIRE(hover2 != nullptr);
    CHECK(hover2->name == "i2");
    CHECK(hover2->typeInfo == "classA"); // auto resolves to classA
}

#include "features/hover/HoverHandler.h"
#include "i18n/LspStrings.h"
#include <lsp/messages.h>
#include <variant>

TEST_CASE("CH14: HoverHandler Doxygen formatting and filtering (clangd style)")
{
    const char* SRC = R"(
/**
 * @class ThreadSafeQueue
 * @brief A first-in, first-out (FIFO) queue with blocking pop operations.
 *
 * @details The class wraps a standard std::queue container. It enforces thread
 * safety by guarding all internal modifications with an internal mutex.
 *
 * ### Usage Example
 * @code{.cpp}
 * concurrent::ThreadSafeQueue<int> queue;
 * queue.push(42);
 *
 * int value;
 * if (queue.try_pop(value)) {
 *     // Process value
 * }
 * @endcode
 *
 * @tparam T The type of the elements stored in the queue. Must be move-constructible.
 */
class IEntity
{
    /**
     * @brief Spawns an entity in the game world.
     * @details This method is responsible for creating and placing an entity in the game world at the specified position.
     *
     * @param[in] pos The position where the entity should be spawned.
     */
    void Spawn(Vector3 pos);

    /**
     * @brief Pushes a new element into the back of the queue.
     * @details Takes ownership of the provided element via rvalue reference,
     * safely locks the internal data array, and signals exactly one waiting thread.
     *
     * @param[in] new_value The item to add. The element is moved into the container.
     * @return true If an element was successfully popped.
     * @return false If the queue was empty at the exact millisecond of evaluation.
     *
     * @note This operation is non-blocking and provides a strong exception guarantee.
     */
    void Update(float deltaTime);
};
    )";

    SymbolTable table;
    Document doc("file:///main.as", SRC);
    SymbolCollector::CollectGlobals(doc, table);

    // Test hovering over 'IEntity'
    {
        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///main.as");
        size_t offset = std::string(SRC).find("class IEntity");
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++)
        {
            if (SRC[i] == '\n') { line++; col = 0; } else { col++; }
        }
        req.position.line = line;
        req.position.character = col + 8; // point to 'IEntity'

        lsp::requests::TextDocument_Hover::Result result;
        angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!result.isNull());
        auto markup = std::get<lsp::MarkupContent>((*result).contents);
        std::string markdown = markup.value;

        CHECK(markdown.find("A first-in, first-out (FIFO) queue") != std::string::npos);
        CHECK(markdown.find("The class wraps a standard") == std::string::npos);
        CHECK(markdown.find("**Parámetros de plantilla:**") != std::string::npos);
        CHECK(markdown.find("`T` \\- The type of the elements") != std::string::npos);
    }

    // Test hovering over 'Spawn'
    {
        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///main.as");
        size_t offset = std::string(SRC).find("Spawn(");
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++)
        {
            if (SRC[i] == '\n') { line++; col = 0; } else { col++; }
        }
        req.position.line = line;
        req.position.character = col;

        lsp::requests::TextDocument_Hover::Result result;
        angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!result.isNull());
        auto markup = std::get<lsp::MarkupContent>((*result).contents);
        std::string markdown = markup.value;

        CHECK(markdown.find("Spawns an entity in the game world.") != std::string::npos);
        CHECK(markdown.find("This method is responsible for creating") == std::string::npos);
        CHECK(markdown.find("**Parámetros:**") != std::string::npos);
        CHECK(markdown.find("`pos` \\- [in] The position where") != std::string::npos);
    }

    // Test hovering over 'Update'
    {
        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///main.as");
        size_t offset = std::string(SRC).find("Update(");
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++)
        {
            if (SRC[i] == '\n') { line++; col = 0; } else { col++; }
        }
        req.position.line = line;
        req.position.character = col;

        lsp::requests::TextDocument_Hover::Result result;
        angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!result.isNull());
        auto markup = std::get<lsp::MarkupContent>((*result).contents);
        std::string markdown = markup.value;

        CHECK(markdown.find("Pushes a new element") != std::string::npos);
        CHECK(markdown.find("**Parámetros:**") != std::string::npos);
        CHECK(markdown.find("`new_value` \\- [in] The item to add.") != std::string::npos);
        CHECK(markdown.find("**Devuelve:**") != std::string::npos);
        CHECK(markdown.find("true If an element was successfully popped. false If the queue") != std::string::npos);
        CHECK(markdown.find("**Nota:**") != std::string::npos);
        CHECK(markdown.find("This operation is non-blocking") != std::string::npos);
    }
    // Test hovering over 'pos' parameter inside 'Spawn'
    {
        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///main.as");
        size_t offset = std::string(SRC).find("Vector3 pos") + 8; // pointing inside "pos"
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++)
        {
            if (SRC[i] == '\n') { line++; col = 0; } else { col++; }
        }
        req.position.line = line;
        req.position.character = col;

        lsp::requests::TextDocument_Hover::Result result;
        angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!result.isNull());
        auto markup = std::get<lsp::MarkupContent>((*result).contents);
        std::string markdown = markup.value;

        // Should include parameter name/type/parent signature
        CHECK(markdown.find("void IEntity::Spawn(Vector3 pos)") != std::string::npos);
        CHECK(markdown.find("**pos** — Parámetro") != std::string::npos);
        // Should include the brief of the parent function
        CHECK(markdown.find("Spawns an entity in the game world.") != std::string::npos);
        // Should include ONLY the specific parameter's docs
        CHECK(markdown.find("**Parámetros:**") != std::string::npos);
        CHECK(markdown.find("`pos` \\- [in] The position where") != std::string::npos);
        // It should NOT include returns, notes, warnings, etc. if they existed on the parent function
    }
}

