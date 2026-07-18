#include <doctest/doctest.h>
#include <tree_sitter/api.h>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

extern "C" TSLanguage* tree_sitter_angelscript();

struct Parser
{
    TSParser* raw = nullptr;
    Parser()
    {
        raw = ts_parser_new();
        printf("ts_parser_new returned %p\n", raw);
        TSLanguage* lang = tree_sitter_angelscript();
        printf("tree_sitter_angelscript returned %p\n", lang);
        bool ok = ts_parser_set_language(raw, lang);
        printf("ts_parser_set_language returned %d\n", ok);
    }
    ~Parser() { ts_parser_delete(raw); }
    TSTree* parse(const std::string& code, TSTree* oldTree = nullptr) const
    {
        printf("parsing string...\n");
        TSTree* res = ts_parser_parse_string(raw, oldTree, code.c_str(), (uint32_t)code.size());
        printf("parsed string, res=%p\n", res);
        return res;
    }
};

struct Tree
{
    TSTree* raw = nullptr;
    explicit Tree(TSTree* t) : raw(t) {}
    ~Tree() { if (raw) ts_tree_delete(raw); }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    TSNode root() const { return ts_tree_root_node(raw); }
};

TEST_SUITE("TreeSitter - Basic Parsing")
{
    TEST_CASE("Simple void function")
    {
        Parser p;
        Tree tree(p.parse("void Main() { }"));
        TSNode root = tree.root();
        CHECK_FALSE(ts_node_has_error(root));
        REQUIRE(ts_node_named_child_count(root) == 1);
        TSNode func = ts_node_named_child(root, 0);
        CHECK(std::string(ts_node_type(func)) == "func_declaration");
    }

    TEST_CASE("AST dump: function inside nested namespace")
    {
        Parser p;
        std::string code =
            "namespace Engine {\n"
            "    namespace Math {\n"
            "        float Lerp(float a, float b, float t) { return a + (b - a) * t; }\n"
            "    }\n"
            "}\n";
        Tree tree(p.parse(code));
        TSNode root = tree.root();

        std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth)
        {
            if (ts_node_is_null(node)) return;
            std::string indent(depth * 2, ' ');
            if (ts_node_is_named(node))
            {
                uint32_t start = ts_node_start_byte(node);
                uint32_t end   = ts_node_end_byte(node);
                std::string src = code.substr(start, end - start);
                if (src.size() > 40) src = src.substr(0, 37) + "...";
                for (auto& ch : src) if (ch == '\n') ch = ' ';
                printf("%s[%s] \"%s\"\n", indent.c_str(), ts_node_type(node), src.c_str());
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); ++i)
            {
                printTree(ts_node_child(node, i), depth + 1);
            }
        };

        printf("\n=== AST: function inside nested namespace ===\n");
        printTree(root, 0);
        printf("=== END AST ===\n\n");

        // This test always passes — its purpose is to print the AST structure
        // so we can identify the exact node type names used for function parameters
        // inside a namespace (e.g. "parameter_list" vs "parameter_list_decl").
        CHECK_FALSE(ts_node_is_null(root));
    }

    TEST_CASE("AST dump: mixin class body and host class body")
    {
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

        std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth)
        {
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

    TEST_CASE("AST dump: broken namespace from user")
    {
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

        std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth)
        {
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

    TEST_CASE("AST dump: local var with args")
    {
        Parser p;
        std::string code = "void Main() { Vector3 v(1, 2, 3); }";
        Tree tree(p.parse(code));
        TSNode root = tree.root();

        std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth)
        {
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
    TEST_CASE("AST dump: typedef and collisions")
    {
        Parser p;
        std::string code = R"(
typedef float MyFloat;
namespace Collision
{
    class Collision {}
    void Collision() {}
    enum Collision { COLLISION_VAL }
}
)";
        Tree tree(p.parse(code));
        TSNode root = tree.root();

        std::function<void(TSNode, int, const char*)> printTree = [&](TSNode node, int depth, const char* fieldName)
        {
            if (ts_node_is_null(node)) return;
            std::string indent(depth * 2, ' ');
            if (fieldName) indent += std::string(fieldName) + ": ";
            uint32_t start = ts_node_start_byte(node);
            uint32_t end   = ts_node_end_byte(node);
            std::string src = code.substr(start, end - start);
            if (src.size() > 50) src = src.substr(0, 47) + "...";
            for (auto& ch : src) if (ch == '\n') ch = ' ';
            if (ts_node_is_named(node))
                printf("%s[%s] \"%s\"\n", indent.c_str(), ts_node_type(node), src.c_str());
            else
                printf("%s(%s)\n", indent.c_str(), ts_node_type(node));
                
            TSTreeCursor cursor = ts_tree_cursor_new(node);
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                do
                {
                    TSNode child = ts_tree_cursor_current_node(&cursor);
                    const char* child_field = ts_tree_cursor_current_field_name(&cursor);
                    printTree(child, depth + 1, child_field);
                } while (ts_tree_cursor_goto_next_sibling(&cursor));
                ts_tree_cursor_goto_parent(&cursor);
            }
            ts_tree_cursor_delete(&cursor);
        };

        printf("\n=== AST: typedef and collisions ===\n");
        printTree(root, 0, nullptr);
        printf("=== END AST ===\n\n");
    }

    TEST_CASE("AST Sandbox")
    {
        TSParser *parser = ts_parser_new();
        ts_parser_set_language(parser, tree_sitter_angelscript());
        
        auto printTree = [](auto& self, TSNode node, const std::string& source, int depth) -> void
        {
            if (ts_node_is_null(node)) return;
            for (int i=0; i<depth; i++) printf("  ");
            
            uint32_t start = ts_node_start_byte(node);
            uint32_t end = ts_node_end_byte(node);
            std::string text = source.substr(start, end - start);
            
            printf("[%s] '%s'\n", ts_node_type(node), text.c_str());
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                self(self, ts_node_child(node, i), source, depth + 1);
            }
        };

        std::string source1 = "class Vector3 { Vector3() {} ~Vector3() {} Vector3(float ax) {} }";
        TSTree *tree1 = ts_parser_parse_string(parser, NULL, source1.c_str(), source1.length());
        printf("\n=== AST: Constructors ===\n");
        printTree(printTree, ts_tree_root_node(tree1), source1, 0);
        printf("=== END AST ===\n\n");
        
        std::string source2 = "class RigidBody { Engine::Math::Vector3 position; }";
        TSTree *tree2 = ts_parser_parse_string(parser, NULL, source2.c_str(), source2.length());
        printf("\n=== AST: Field ===\n");
        printTree(printTree, ts_tree_root_node(tree2), source2, 0);
        printf("=== END AST ===\n\n");
        
        ts_tree_delete(tree1);
        ts_tree_delete(tree2);
        ts_parser_delete(parser);
    }
}

TEST_CASE("AST dump of interface")
{
    const char* SRC = R"(
interface IEntity {
    /**
     * @brief Spawns an entity in the game world.
     * @details This method is responsible for creating and placing an entity in the game world at the specified position.
     *
     * @param[in] pos The position where the entity should be spawned.
     */
    void Spawn(Vector3 pos);
    /**
     * @brief Removes an entity from the game world.
     * @details This method is responsible for removing or deactivating an entity from the game world, ensuring that all associated resources are properly released and that the entity is no longer active in the simulation.
     */
    void Despawn();
}
)";

    Parser p;
    Tree tree(p.parse(SRC));
    TSNode root = tree.root();

    std::function<void(TSNode, int)> printTree = [&](TSNode node, int depth)
    {
        if (ts_node_is_null(node)) return;
        std::string indent(depth * 2, ' ');
        if (ts_node_is_named(node))
        {
            uint32_t start = ts_node_start_byte(node);
            uint32_t end   = ts_node_end_byte(node);
            std::string src = std::string(SRC).substr(start, end - start);
            if (src.size() > 40) src = src.substr(0, 37) + "...";
            for (auto& ch : src) if (ch == '\n') ch = ' ';
            printf("%s[%s] \"%s\"\n", indent.c_str(), ts_node_type(node), src.c_str());
        }
        for (uint32_t i = 0; i < ts_node_child_count(node); ++i)
        {
            printTree(ts_node_child(node, i), depth + 1);
        }
    };

    printf("\n=== AST: Interface ===\n");
    printTree(root, 0);
    printf("=== END AST ===\n\n");
}

TEST_CASE("AST dump of full_scratch.as")
{
    Parser p;
    // read file
    FILE* f = fopen("tests/full_scratch.as", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string code;
    code.resize(fsize);
    fread(code.data(), 1, fsize, f);
    fclose(f);

    Tree tree(p.parse(code));
    TSNode root = tree.root();
        FILE* out = fopen("ast_dump_out.txt", "w");
        
        struct Context
        {
            std::string prefix;
            bool isLast;
        };

        std::function<void(TSNode, const std::string&, bool, const char*)> printTree = 
        [&](TSNode node, const std::string& prefix, bool isLast, const char* fieldName)
        {
            if (ts_node_is_null(node)) return;
            
            // Print current node
            std::string branch = isLast ? "└── " : "├── ";
            fprintf(out, "%s%s", prefix.c_str(), branch.c_str());
            
            if (fieldName)
            {
                fprintf(out, "%s: ", fieldName);
            }
            
            fprintf(out, "[%s]", ts_node_type(node));
            
            // Only print source text for leaf nodes or specific named nodes to avoid clutter
            bool isLeaf = ts_node_child_count(node) == 0;
            if (isLeaf || std::string_view(ts_node_type(node)) == "identifier" || std::string_view(ts_node_type(node)) == "type_identifier" || std::string_view(ts_node_type(node)) == "primitive_type")
            {
                uint32_t start = ts_node_start_byte(node);
                uint32_t end   = ts_node_end_byte(node);
                std::string src = code.substr(start, end - start);
                if (src.size() > 40) src = src.substr(0, 37) + "...";
                for (auto& ch : src) if (ch == '\n' || ch == '\r') ch = ' ';
                fprintf(out, " \"%s\"", src.c_str());
            }
            fprintf(out, "\n");

            // Print children
            uint32_t childCount = ts_node_child_count(node);
            std::string childPrefix = prefix + (isLast ? "    " : "│   ");
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char* childField = ts_node_field_name_for_child(node, i);
                printTree(child, childPrefix, i == childCount - 1, childField);
            }
        };
        
        fprintf(out, "AST ROOT:\n");
        printTree(root, "", true, nullptr);
        fclose(out);
}

TEST_CASE("AST dump: full_scratch")
{
    Parser p;
    std::string code = R"( // =============================================================================
// scratch.as — Script de pruebas complejo para el LSP de AngelScript
//
// INSTRUCCIONES:
//   Pasa el cursor sobre cualquier identificador resaltado para probar el hover.
//   Cada sección tiene comentarios indicando qué casos se prueban.
// =============================================================================

// =============================================================================
// SECCIÓN 1: Namespaces anidados y tipos cualificados
// =============================================================================

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

            /// Retorna la longitud del vector.
            float Length() { return x * x + y * y + z * z; }

            Vector3 opAdd(Vector3 other) { return Vector3(x + other.x, y + other.y, z + other.z); }
            Vector3 opMul(float s)       { return Vector3(x * s, y * s, z * s); }
        }

        class Quaternion
        {
            float w;
            float x;
            float y;
            float z;
        }

        /// Interpolación lineal entre dos floats.
        float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        /// Interpola entre dos vectores.
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

    namespace Events
    {
        funcdef void EventCallback(int eventId, float data);
    }
}

// =============================================================================
// SECCIÓN 2: Enums
// =============================================================================

enum GameState
{
    GS_MENU,
    GS_PLAYING,
    GS_PAUSED,
    GS_GAMEOVER
}

enum DamageType
{
    DMG_PHYSICAL,
    DMG_FIRE,
    DMG_POISON,
    DMG_MAGIC
}

// =============================================================================
// SECCIÓN 3: Clases con herencia múltiple y mixins
// =============================================================================

/// Entidad base con puntos de vida.
class Entity
{
    float hp;
    float maxHp;
    string name;

    Entity()
    {
        hp    = 100.0f;
        maxHp = 100.0f;
        name  = string();
    }

    /// Aplica daño a la entidad.
    void TakeDamage(float amount, DamageType dmgType)
    {
        hp -= amount;
        if (hp < 0.0f) hp = 0.0f;
    }

    bool IsAlive() { return hp > 0.0f; }
}

/// Entidad con un transform en el espacio.
class Spatial : Entity
{
    Engine::Math::Vector3 position;
    Engine::Math::Vector3 scale;

    Spatial()
    {
        position = Engine::Math::Vector3(0, 0, 0);
        scale    = Engine::Math::Vector3(1, 1, 1);
    }

    void MoveTo(Engine::Math::Vector3 target)
    {
        position = target;
    }
}

// ---- MIXINS ----

/// Mixin que otorga regeneración de vida.
mixin class Regenerator
{
    float regenRate = 1.0f;

    /// Regenera hp del objeto anfitrión cada tick.
    void Regen()
    {
        // 'hp' se asume del objeto anfitrión (Entity o subclase).
        // TEST: hover sobre 'hp' debe mostrar las clases que lo proveen.
        hp = hp + regenRate;
        if (hp > maxHp) hp = maxHp;
    }
}

/// Mixin que otorga habilidades de ataque cuerpo a cuerpo.
mixin class Melee
{
    float attackDamage = 10.0f;
    float attackRange  = 1.5f;

    /// Golpea un objetivo cercano.
    void Strike(Entity@ target)
    {
        // 'attackDamage' pertenece a este mixin.
        // TEST: hover sobre 'target' debe mostrar "Entity@ target — Parameter"
        target.TakeDamage(attackDamage, DMG_PHYSICAL);
    }
}

/// Mixin de movimiento.
mixin class Mover
{
    float speed = 5.0f;

    void Move(Engine::Math::Vector3 direction)
    {
        // TEST: hover sobre 'speed' — propiedad del mixin.
        // TEST: hover sobre 'position' — propiedad de Spatial (anfitrión).
        position = position + (direction * speed);
    }
}

// ---- Clases que usan Mixins ----

/// Troll: hereda Entity y usa Regenerator + Melee.
class Troll : Entity, Regenerator, Melee
{
    int angerLevel;

    Troll()
    {
        name        = string();
        hp          = 250.0f;
        maxHp       = 250.0f;
        regenRate   = 3.0f;
        attackDamage = 20.0f;
        angerLevel  = 0;
    }

    void Enrage()
    {
        // TEST: hover sobre 'regenRate' → propiedad de Regenerator.
        regenRate *= 2.0f;
        angerLevel++;
    }
}

/// Ogro: hereda Entity y usa Regenerator.
class Ogre : Entity, Regenerator
{
    Ogre()
    {
        name      = string();
        hp        = 180.0f;
        maxHp     = 180.0f;
        regenRate = 1.5f;
    }
}

/// Jugador: hereda Spatial y usa Regenerator + Melee + Mover.
class Player : Spatial, Regenerator, Melee, Mover
{
    int score;
    GameState currentState;

    Player()
    {
        name        = string();
        hp          = 100.0f;
        maxHp       = 100.0f;
        regenRate   = 0.5f;
        attackDamage = 15.0f;
        speed       = 7.0f;
        score       = 0;
        currentState = GS_PLAYING;
    }

    void Update(float deltaTime)
    {
        // TEST: hover sobre 'regenRate' → Regenerator.
        // TEST: hover sobre 'score'     → Player.
        // TEST: hover sobre 'deltaTime' → Parameter.
        Regen();
        score += int(deltaTime * 10.0f);
    }
}

// =============================================================================
// SECCIÓN 4: Interfaces
// =============================================================================

interface IRenderable
{
    void Draw();
    string GetMeshPath();
}

interface ICollidable
{
    bool CheckCollision(Entity@ other);
    float GetRadius();
}

/// Enemigo que implementa múltiples interfaces.
class Boss : Spatial, Regenerator, Melee, IRenderable, ICollidable
{
    string meshPath;
    float  collisionRadius;

    Boss()
    {
        name             = string();
        hp               = 1000.0f;
        maxHp            = 1000.0f;
        regenRate        = 5.0f;
        attackDamage      = 50.0f;
        meshPath         = string();
        collisionRadius  = 3.0f;
    }

    void Draw()
    {
        // Lógica de renderizado.
    }

    string GetMeshPath() { return meshPath; }

    bool CheckCollision(Entity@ other)
    {
        // TEST: hover sobre 'other' → "Entity@ other — Parameter".
        return other.IsAlive();
    }

    float GetRadius() { return collisionRadius; }
}

// =============================================================================
// SECCIÓN 5: Funciones globales con namespaces y 'using namespace'
// =============================================================================

using namespace Engine::Math;

/// Calcula el punto medio entre dos posiciones.
Vector3 Midpoint(Vector3 a, Vector3 b)
{
    // TEST: hover sobre 'a' y 'b' → parámetro tipo Vector3.
    return VectorLerp(a, b, 0.5f);
}

/// Comprueba si una entidad está en rango de otra.
bool IsInRange(Entity@ source, Entity@ target, float maxRange)
{
    // TEST: hover sobre 'source', 'target', 'maxRange' → Parámetros.
    return source.IsAlive() && target.IsAlive();
}

// =============================================================================
// SECCIÓN 6: Funciones anónimas (funcdef) y callbacks
// =============================================================================

funcdef void TimerCallback(float elapsed);
funcdef bool FilterPredicate(Entity@ entity);

class EventSystem
{
    TimerCallback@ timerHandler;
    FilterPredicate@ filter;

    void RegisterTimer(TimerCallback@ cb)
    {
        // TEST: hover sobre 'cb' → "TimerCallback@ cb — Parameter".
        @timerHandler = cb;
    }

    void Tick(float dt)
    {
        if (timerHandler !is null)
            timerHandler(dt);
    }
}

// =============================================================================
// SECCIÓN 7: Variables locales complejas en Main
// =============================================================================

void Main()
{
    // TEST: hover sobre 'myTroll'         → "Troll myTroll — Variable"
    Troll myTroll;

    // TEST: hover sobre 'player'          → "Player player — Variable"
    Player player;

    // TEST: hover sobre 'state'           → "GameState state — Variable"
    GameState state = GS_PLAYING;

    // TEST: hover sobre 'dmg'             → "DamageType dmg — Variable"
    DamageType dmg = DMG_FIRE;

    // TEST: hover sobre 'pos'             → "Vector3 pos — Variable" (con using namespace)
    Vector3 pos;
    pos.x = Lerp(0.0f, 10.0f, 0.5f);

    // TEST: hover sobre 'pos2'            → "Engine::Math::Vector3 pos2 — Variable" (cualificado)
    Engine::Math::Vector3 pos2;
    pos2.y = Engine::Math::Lerp(0.0f, 10.0f, 0.5f);

    // TEST: hover sobre 'quat'            → "Engine::Math::Quaternion quat — Variable"
    Engine::Math::Quaternion quat;

    // TEST: hover sobre 'body'            → "Engine::Physics::RigidBody body — Variable"
    Engine::Physics::RigidBody body;
    body.mass = 75.0f;
    body.ApplyForce(Engine::Math::Vector3(0, -9.8f, 0));

    // TEST: hover sobre 'events'          → "EventSystem events — Variable"
    EventSystem events;

    // TEST: hover sobre 'mid'             → "Vector3 mid — Variable"
    Vector3 mid = Midpoint(pos, pos2);

    // TEST: hover sobre 'myTroll.TakeDamage' → método de Entity
    myTroll.TakeDamage(15.0f, DMG_PHYSICAL);

    // TEST: hover sobre 'myTroll.Regen'   → método de Regenerator
    myTroll.Regen();

    // TEST: hover sobre 'player.Move'     → método de Mover
    player.Move(Vector3(1.0f, 0.0f, 0.0f));

    // TEST: hover sobre 'player.currentState' → propiedad de Player (GameState)
    player.currentState = GS_PAUSED;

    // TEST: hover sobre 'state' (GS_GAMEOVER) → enum member de GameState
    state = GS_GAMEOVER;

    // Condicional con variable local en scope limitado
    if (player.IsAlive())
    {
        // TEST: hover sobre 'damageDealt' → "float damageDealt — Variable"
        float damageDealt = 0.0f;
        player.Update(0.016f);
        damageDealt += 5.0f;
    }

    // Variable en bucle
    for (int i = 0; i < 10; i++)
    {
        // TEST: hover sobre 'i'           → "int i — Variable"
        // TEST: hover sobre 'step'        → "float step — Variable"
        float step = float(i) * 0.1f;
        pos.x += step;
    }
}

// =============================================================================
// SECCIÓN 8: Herencia profunda y shadowing de propiedades
// =============================================================================

/// BossFinal hereda Boss y sombrea la propiedad 'hp' de Entity.
class FinalBoss : Boss
{
    /// hp propio de FinalBoss — sombrea al de Entity.
    // float hp;

    FinalBoss()
    {
        // TEST: Hover sobre 'hp' aquí → ¿resuelve a FinalBoss.hp o a Entity.hp?
        hp          = 9999.0f;
        maxHp       = 9999.0f;
        regenRate   = 10.0f;
        attackDamage = 100.0f;
    }

    void Phase2()
    {
        // TEST: hover sobre 'hp' → debe mostrar FinalBoss.hp (shadowing)
        hp *= 0.5f;
    }
}

// =============================================================================
// SECCIÓN 9: Shared classes y tipos externos
// =============================================================================

shared class SharedResource
{
    int id;
    string resourcePath;

    SharedResource(int rid, string path)
    {
        id           = rid;
        resourcePath = path;
    }

    string GetPath() { return resourcePath; }
}

class ExternalTexture
{
    int width;
    int height;
    string format;
}

void LoadAssets()
{
    // TEST: hover sobre 'tex'      → "ExternalTexture tex — Variable"
    ExternalTexture tex;

    // TEST: hover sobre 'res'      → "SharedResource@ res — Variable"
    SharedResource@ res = SharedResource(1, string());

    // TEST: hover sobre 'path'     → "string path — Variable"
    string path = res.GetPath();
}
 )";
    Tree tree(p.parse(code));
    TSNode root = tree.root();
    CHECK_FALSE(ts_node_is_null(root));
    bool hasError = false;
    std::function<void(TSNode)> checkError = [&](TSNode node)
    {
        if (std::string_view(ts_node_type(node)) == "ERROR") hasError = true;
        for (uint32_t i = 0; i < ts_node_child_count(node); i++) checkError(ts_node_child(node, i));
    };
    checkError(root);
    CHECK_FALSE(hasError);
}

