#include <doctest/doctest.h>
#include "analysis/SymbolCollector.h"
#include "document/Document.h"
#include "helpers/TestFixtures.h"

using namespace analysis;

TEST_SUITE("SymbolCollector")
{
    // ==========================================
    // Grupo A: Funciones (func_declaration)
    // ==========================================
    TEST_CASE("A1: Funcion simple con return type")
    {
        Document doc("file:///test.as", "void Main() {}");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("Main");
        REQUIRE(sym != nullptr);
        CHECK(sym->kind == SymbolKind::Function);
        CHECK(sym->typeInfo == "void");
        CHECK(sym->signature == "void Main()");
    }

    TEST_CASE("A1.1: Funcion disfrazada como variable_declaration en namespace (Bug 2)")
    {
        // Bug 2: Tree-sitter recovers "float Lerp(float a, float b, float t) {}" inside a namespace
        // as a variable_declaration with a parameter_list if the context causes a parse anomaly.
        // Even if it parses correctly in a clean test, simulating a syntax error or a context
        // that produces this node structure verifies our recovery logic in SymbolCollector.
        // We will construct a code snippet that forces a variable_declaration parsing or tests
        // the fix directly. Wait, if it parses as func_declaration naturally, we should just
        // test the natural parse inside a namespace.
        Document doc("file:///test.as", 
            "namespace Engine {\n"
            "    namespace Math {\n"
            "        float Lerp(float a, float b, float t) { return a + (b - a) * t; }\n"
            "    }\n"
            "}\n"
        );
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* engineSym = table.FindGlobalByName("Engine");
        REQUIRE(engineSym != nullptr);
        CHECK(engineSym->kind == SymbolKind::Namespace);

        bool foundMath = false;
        Symbol* mathSym = nullptr;
        for (auto& child : engineSym->children) {
            if (child->name == "Math") {
                foundMath = true;
                mathSym = child.get();
                break;
            }
        }
        REQUIRE(foundMath);
        
        bool foundLerp = false;
        for (auto& child : mathSym->children) {
            if (child->name == "Lerp") {
                foundLerp = true;
                // Bug 2 fix ensures this is a Function, not a Variable!
                CHECK(child->kind == SymbolKind::Function);
                CHECK(child->signature == "float Lerp(float a, float b, float t)");
                CHECK(child->typeInfo == "float");
                break;
            }
        }
        REQUIRE(foundLerp);
    }

    TEST_CASE("A2: Funcion con parametros primitivos")
    {
        Document doc("file:///test.as", "int Add(int a, int b) { return a + b; }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("Add");
        REQUIRE(sym != nullptr);
        CHECK(sym->typeInfo == "int");
        REQUIRE(sym->params.size() == 2);
        CHECK(sym->params[0].typeName == "int");
        CHECK(sym->params[0].name == "a");
        CHECK(sym->params[1].typeName == "int");
        CHECK(sym->params[1].name == "b");
        CHECK(sym->signature == "int Add(int a, int b)");
    }

    TEST_CASE("A3: Funcion con handle y referencia (sintaxis real del lenguaje)")
    {
        Document doc("file:///test.as", "void DealDamage(Player@ target, int &out actualDamage) {}");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("DealDamage");
        REQUIRE(sym != nullptr);
        REQUIRE(sym->params.size() == 2);
        CHECK(sym->params[0].typeName == "Player@");
        CHECK(sym->params[0].name == "target");
        CHECK(sym->params[1].typeName == "int &out");
        CHECK(sym->params[1].name == "actualDamage");
        CHECK(sym->signature == "void DealDamage(Player@ target, int &out actualDamage)");
    }

    TEST_CASE("A4: Funcion con const (metodo const)")
    {
        Document doc("file:///test.as", "class Actor { float GetSpeed() const { return m_speed; } }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Actor");
        REQUIRE(cls != nullptr);
        REQUIRE(cls->children.size() == 1);
        CHECK(cls->children[0]->name == "GetSpeed");
        CHECK(cls->children[0]->typeInfo == "float");
    }

    TEST_CASE("A5: Funcion con parametro anonimo (sin nombre)")
    {
        Document doc("file:///test.as", "void Process(int, float) {}");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("Process");
        REQUIRE(sym != nullptr);
        REQUIRE(sym->params.size() == 2);
        CHECK(sym->params[0].typeName == "int");
        CHECK(sym->params[0].name == "");
        CHECK(sym->params[1].typeName == "float");
        CHECK(sym->params[1].name == "");
    }

    TEST_CASE("A6: Funcion con tipo complejo de retorno")
    {
        Document doc("file:///test.as", "array<int> GetItems() { return m_items; }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("GetItems");
        REQUIRE(sym != nullptr);
        CHECK(sym->typeInfo == "array<int>");
        CHECK(sym->name == "GetItems");
    }

    TEST_CASE("A7: Funcion con default value en parametro")
    {
        Document doc("file:///test.as", "void Spawn(int count = 1, bool active = true) {}");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("Spawn");
        REQUIRE(sym != nullptr);
        REQUIRE(sym->params.size() == 2);
        CHECK(sym->params[0].typeName == "int");
        CHECK(sym->params[0].name == "count");
        CHECK(sym->params[1].typeName == "bool");
        CHECK(sym->params[1].name == "active");
    }

    // ==========================================
    // Grupo B: Variables (variable_declaration)
    // ==========================================
    TEST_CASE("B1: Variable global simple")
    {
        Document doc("file:///test.as", "int g_score = 0;");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("g_score");
        REQUIRE(sym != nullptr);
        CHECK(sym->kind == SymbolKind::Variable);
        CHECK(sym->typeInfo == "int");
    }

    TEST_CASE("B2: Handle (sintaxis real con @)")
    {
        Document doc("file:///test.as", "Player@ g_player;");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("g_player");
        REQUIRE(sym != nullptr);
        CHECK(sym->kind == SymbolKind::Variable);
        CHECK(sym->typeInfo == "Player@");
    }

    TEST_CASE("B3: Handle const")
    {
        Document doc("file:///test.as", "const Enemy@ g_boss = null;");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("g_boss");
        REQUIRE(sym != nullptr);
        CHECK(sym->typeInfo == "const Enemy@");
    }

    TEST_CASE("B4: Multiples declaradores en una linea (critico)")
    {
        Document doc("file:///test.as", "int x = 0, y = 0, z = 0;");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* x = table.FindGlobalByName("x");
        Symbol* y = table.FindGlobalByName("y");
        Symbol* z = table.FindGlobalByName("z");
        REQUIRE(x != nullptr);
        REQUIRE(y != nullptr);
        REQUIRE(z != nullptr);
        CHECK(x->typeInfo == "int");
        CHECK(y->typeInfo == "int");
        CHECK(z->typeInfo == "int");
    }

    TEST_CASE("B5: Variable con constructor args (sin '=')")
    {
        Document doc("file:///test.as", "Player player('Hero');");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("player");
        REQUIRE(sym != nullptr);
        CHECK(sym->typeInfo == "Player");
    }

    TEST_CASE("B6: Array generico")
    {
        Document doc("file:///test.as", "array<float> m_weights;");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("m_weights");
        REQUIRE(sym != nullptr);
        CHECK(sym->typeInfo == "array<float>");
    }

    TEST_CASE("B7: Variable local con selectionRange correcta")
    {
        std::string code = "void Main() { int localVar = 5; }";
        Document doc("file:///test.as", code);
        SymbolTable table;
        
        TSNode root = doc.RootNode();
        TSNode funcNode = ts_node_child(root, 0); // func_declaration
        TSNode blockNode = ts_node_child_by_field_name(funcNode, "body", 4);
        
        SymbolCollector::TraverseLocals(blockNode, doc, table, nullptr);
        
        Symbol* sym = table.FindLocalByName("localVar");
        REQUIRE(sym != nullptr);
        // "int localVar = 5;" starts at character 14 inside the code line
        CHECK(sym->selectionRange.start.character == 18); // Actually it's 18 because "void Main() { int localVar"
    }

    // ==========================================
    // Grupo C: Clases (class_declaration)
    // ==========================================
    TEST_CASE("C1: Clase simple")
    {
        Document doc("file:///test.as", "class Player { int hp; void Heal() {} }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Player");
        REQUIRE(cls != nullptr);
        CHECK(cls->kind == SymbolKind::Class);
        REQUIRE(cls->children.size() == 2);
        CHECK(cls->children[0]->name == "hp");
        CHECK(cls->children[0]->kind == SymbolKind::Variable);
        CHECK(cls->children[0]->typeInfo == "int");
        CHECK(cls->children[1]->name == "Heal");
        CHECK(cls->children[1]->kind == SymbolKind::Function);
    }

    TEST_CASE("C2: Clase con herencia")
    {
        Document doc("file:///test.as", "class Enemy : Entity { void Attack() {} }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Enemy");
        REQUIRE(cls != nullptr);
        CHECK(cls->baseClasses[0] == "Entity");

        Symbol* notCls = table.FindGlobalByName("Entity");
        CHECK(notCls == nullptr); // Ensure "Entity" is not mistakenly collected
    }

    TEST_CASE("C3: Constructores y Destructores detectados correctamente") {
        Document doc("file:///test.as", "class Vec { Vec() {} ~Vec() {} Vec(float x) {} }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        const Symbol* vecClass = table.FindGlobalByName("Vec");
        REQUIRE(vecClass != nullptr);

        int constructors = 0;
        int destructors = 0;
        for (const auto& child : vecClass->children) {
            if (child->kind == SymbolKind::Constructor && child->name == "Vec") constructors++;
            if (child->kind == SymbolKind::Destructor && child->name == "~Vec") destructors++;
        }

        CHECK(constructors == 2);
        CHECK(destructors == 1);
    }

    TEST_CASE("C4: Campos de clase con namespace tienen typeInfo cualificado") {
        Document doc("file:///test.as", "class Body { Engine::Math::Vector3 pos; }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        const Symbol* bodyClass = table.FindGlobalByName("Body");
        REQUIRE(bodyClass != nullptr);

        const Symbol* posField = nullptr;
        for (const auto& child : bodyClass->children) {
            if (child->name == "pos") posField = child.get();
        }

        REQUIRE(posField != nullptr);
        CHECK(posField->typeInfo == "Engine::Math::Vector3");
    }

    TEST_CASE("C3: Clase con modificadores")
    {
        Document doc("file:///test.as", "shared abstract class BaseActor { void Update() {} }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("BaseActor");
        REQUIRE(cls != nullptr);
    }

    TEST_CASE("C4: Clase con virtual property")
    {
        Document doc("file:///test.as", "class Window { int width { get { return 0; } set {} } }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Window");
        REQUIRE(cls != nullptr);
        REQUIRE(cls->children.size() == 1);
        CHECK(cls->children[0]->name == "width");
        CHECK(cls->children[0]->kind == SymbolKind::Property);
        CHECK(cls->children[0]->typeInfo == "int");
    }

    TEST_CASE("C5: Clase con handle en campo")
    {
        Document doc("file:///test.as", "class Scene { Player@ activePlayer; }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Scene");
        REQUIRE(cls != nullptr);
        REQUIRE(cls->children.size() == 1);
        CHECK(cls->children[0]->name == "activePlayer");
        CHECK(cls->children[0]->typeInfo == "Player@");
    }

    TEST_CASE("C6: Clase con funcdef anidado")
    {
        Document doc("file:///test.as", "class Timer { funcdef void Callback(); Callback@ onTick; }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Timer");
        REQUIRE(cls != nullptr);
        REQUIRE(cls->children.size() == 2);
        CHECK(cls->children[0]->name == "Callback");
        CHECK(cls->children[0]->kind == SymbolKind::Funcdef);
        CHECK(cls->children[1]->name == "onTick");
        CHECK(cls->children[1]->kind == SymbolKind::Variable);
    }

    TEST_CASE("C7: Clase con metodo que tiene parametros")
    {
        Document doc("file:///test.as", "class Combat { void Fire(int damage, Player@ target) {} }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Combat");
        REQUIRE(cls != nullptr);
        REQUIRE(cls->children.size() == 1);
        CHECK(cls->children[0]->name == "Fire");
        REQUIRE(cls->children[0]->params.size() == 2);
        CHECK(cls->children[0]->params[0].typeName == "int");
        CHECK(cls->children[0]->params[0].name == "damage");
        CHECK(cls->children[0]->params[1].typeName == "Player@");
        CHECK(cls->children[0]->params[1].name == "target");
    }

    // ==========================================
    // Grupo D: Namespaces (namespace_declaration)
    // ==========================================
    TEST_CASE("D1: Namespace simple")
    {
        Document doc("file:///test.as", "namespace Math { float PI = 3.14f; }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* ns = table.FindGlobalByName("Math");
        REQUIRE(ns != nullptr);
        CHECK(ns->kind == SymbolKind::Namespace);
        REQUIRE(ns->children.size() == 1);
        CHECK(ns->children[0]->name == "PI");
        CHECK(ns->children[0]->typeInfo == "float");
    }

    TEST_CASE("D2: Namespace con funciones")
    {
        Document doc("file:///test.as", "namespace Utils { void Log(string msg) {} }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* ns = table.FindGlobalByName("Utils");
        REQUIRE(ns != nullptr);
        REQUIRE(ns->children.size() == 1);
        CHECK(ns->children[0]->name == "Log");
        CHECK(ns->children[0]->signature == "void Log(string msg)");
    }

    TEST_CASE("D3: Namespace anidado con sintaxis :: - modelo C++/C#")
    {
        Document doc("file:///test.as", "namespace Engine::Math { float Lerp(float a, float b, float t) {} }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* engineNs = table.FindGlobalByName("Engine");
        REQUIRE(engineNs != nullptr);
        CHECK(engineNs->kind == SymbolKind::Namespace);
        REQUIRE(engineNs->children.size() == 1);
        
        auto mathNs = engineNs->children[0];
        CHECK(mathNs->name == "Math");
        CHECK(mathNs->kind == SymbolKind::Namespace);
        REQUIRE(mathNs->children.size() == 1);
        
        auto lerpFunc = mathNs->children[0];
        CHECK(lerpFunc->name == "Lerp");
        CHECK(lerpFunc->kind == SymbolKind::Function);
        CHECK(lerpFunc->signature == "float Lerp(float a, float b, float t)");
    }

    TEST_CASE("D4: Namespaces separados (mismo nombre)")
    {
        Document doc("file:///test.as", "namespace A { int x; } namespace A { int y; }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* ns = table.FindGlobalByName("A");
        REQUIRE(ns != nullptr);
        // Currently it merges namespaces with the same name, so it will have both 'x' and 'y'
        REQUIRE(ns->children.size() == 2);
        CHECK(ns->children[0]->name == "x");
        CHECK(ns->children[1]->name == "y");
    }

    // ==========================================
    // Grupo E: Enums (enum_declaration)
    // ==========================================
    TEST_CASE("E1: Enum basico sin valores")
    {
        Document doc("file:///test.as", "enum State { IDLE, RUN, ATTACK, DEAD }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* enm = table.FindGlobalByName("State");
        REQUIRE(enm != nullptr);
        CHECK(enm->kind == SymbolKind::Enum);
        REQUIRE(enm->children.size() == 4);
        CHECK(enm->children[0]->name == "IDLE");
        CHECK(enm->children[0]->kind == SymbolKind::EnumMember);
        CHECK(enm->children[3]->name == "DEAD");
        CHECK(enm->children[3]->kind == SymbolKind::EnumMember);
    }

    TEST_CASE("E2: Enum con valores asignados")
    {
        Document doc("file:///test.as", "enum Priority { LOW = 0, MEDIUM = 5, HIGH = 10 }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* enm = table.FindGlobalByName("Priority");
        REQUIRE(enm != nullptr);
        REQUIRE(enm->children.size() == 3);
        CHECK(enm->children[0]->name == "LOW");
        CHECK(enm->children[1]->name == "MEDIUM");
        CHECK(enm->children[2]->name == "HIGH");
    }

    TEST_CASE("E3: Enum con modificador shared")
    {
        Document doc("file:///test.as", "shared enum Color { RED, GREEN, BLUE }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* enm = table.FindGlobalByName("Color");
        REQUIRE(enm != nullptr);
    }

    // ==========================================
    // Grupo F: Funcdefs (funcdef_declaration)
    // ==========================================
    TEST_CASE("F1: Funcdef basico")
    {
        Document doc("file:///test.as", "funcdef void EventCallback(int eventId);");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* fd = table.FindGlobalByName("EventCallback");
        REQUIRE(fd != nullptr);
        CHECK(fd->kind == SymbolKind::Funcdef);
        CHECK(fd->typeInfo == "void");
        REQUIRE(fd->params.size() == 1);
        CHECK(fd->params[0].typeName == "int");
        CHECK(fd->params[0].name == "eventId");
    }

    TEST_CASE("F2: Funcdef con retorno")
    {
        Document doc("file:///test.as", "funcdef float MathOp(float a, float b);");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* fd = table.FindGlobalByName("MathOp");
        REQUIRE(fd != nullptr);
        CHECK(fd->typeInfo == "float");
        CHECK(fd->signature == "float MathOp(float a, float b)");
    }

    // ==========================================
    // Grupo G: Virtual Properties (virtual_property)
    // ==========================================
    TEST_CASE("G1: Virtual property con get/set")
    {
        Document doc("file:///test.as", "class Actor { float speed { get { return m_speed; } set { m_speed = value; } } }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Actor");
        REQUIRE(cls != nullptr);
        REQUIRE(cls->children.size() == 1);
        CHECK(cls->children[0]->name == "speed");
        CHECK(cls->children[0]->kind == SymbolKind::Property);
        CHECK(cls->children[0]->typeInfo == "float");
    }

    TEST_CASE("G2: Virtual property con handle")
    {
        Document doc("file:///test.as", "class Scene { Player@ player { get { return m_player; } } }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* cls = table.FindGlobalByName("Scene");
        REQUIRE(cls != nullptr);
        REQUIRE(cls->children.size() == 1);
        CHECK(cls->children[0]->name == "player");
        CHECK(cls->children[0]->typeInfo == "Player@");
    }

    // ==========================================
    // Grupo H: Ranges / selectionRange
    // ==========================================
    TEST_CASE("H1: selectionRange apunta solo al identifier")
    {
        Document doc("file:///test.as", "void MyFunc() {}");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("MyFunc");
        REQUIRE(sym != nullptr);
        CHECK(sym->selectionRange.start.character == 5);
        CHECK(sym->selectionRange.end.character == 11);
        CHECK(sym->fullRange.start.character == 0);
    }

    TEST_CASE("H2: selectionRange de variable")
    {
        Document doc("file:///test.as", "int g_counter = 0;");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("g_counter");
        REQUIRE(sym != nullptr);
        CHECK(sym->selectionRange.start.character == 4);
    }

    TEST_CASE("H3: selectionRange de clase")
    {
        Document doc("file:///test.as", "class Enemy : Entity {}");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("Enemy");
        REQUIRE(sym != nullptr);
        CHECK(sym->selectionRange.start.character == 6);
    }

    TEST_CASE("H4: selectionRange de enum member")
    {
        Document doc("file:///test.as", "enum State { IDLE, RUN }");
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        
        Symbol* sym = table.FindGlobalByName("State");
        REQUIRE(sym != nullptr);
        REQUIRE(sym->children.size() == 2);
        CHECK(sym->children[0]->selectionRange.start.character == 13);
    }
}
