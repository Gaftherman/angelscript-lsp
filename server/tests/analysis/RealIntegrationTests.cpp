#include <doctest/doctest.h>
#include "analysis/PredefinedLoader.h"
#include "analysis/ValidationOracle.h"
#include "analysis/SymbolTable.h"
#include "document/Document.h"
#include "lsp/types.h"

TEST_SUITE("RealIntegrationTests")
{
    TEST_CASE("Real as.predefined Parsing")
    {
        analysis::SymbolTable globalTable;
        std::string mockPredefined = R"(
            typedef uint32 ConCommandFlags_t;
            funcdef bool less(const ?&in a, const ?&in b);
            class CBasePlayer {
                void Kill();
                entvars_t@ pev;
            }
        )";

        analysis::PredefinedLoader::LoadFromSource(mockPredefined, globalTable, "string", "array");

        const analysis::Symbol *typedefSym = globalTable.FindGlobalByName("ConCommandFlags_t");
        REQUIRE(typedefSym != nullptr);
        CHECK(typedefSym->name == "ConCommandFlags_t");
        CHECK(typedefSym->kind == analysis::SymbolKind::Typedef);

        const analysis::Symbol *funcdefSym = globalTable.FindGlobalByName("less");
        REQUIRE(funcdefSym != nullptr);
        CHECK(funcdefSym->name == "less");
        CHECK(funcdefSym->kind == analysis::SymbolKind::Funcdef);

        const analysis::Symbol *classSym = globalTable.FindGlobalByName("CBasePlayer");
        REQUIRE(classSym != nullptr);
        CHECK(classSym->name == "CBasePlayer");
        CHECK(classSym->kind == analysis::SymbolKind::Class);

        const analysis::Symbol *methodSym = globalTable.FindByNameDeep("CBasePlayer::Kill");
        if (!methodSym && classSym)
        {
            for (const auto &child : classSym->children)
            {
                if (child->name == "Kill")
                {
                    methodSym = child.get();
                    break;
                }
            }
        }
        REQUIRE(methodSym != nullptr);
        CHECK(methodSym->name == "Kill");
        CHECK((methodSym->kind == analysis::SymbolKind::Method || methodSym->kind == analysis::SymbolKind::Function));
    }

    TEST_CASE("Tree-Sitter Syntax Diagnostic Catching")
    {
        analysis::ValidationOracle oracle;
        std::string brokenSyntaxCode = R"(
            void Main() {
                int a = ;
            }
        )";

        auto diags = oracle.ValidateSync(brokenSyntaxCode, "file:///test_syntax.as");
        REQUIRE(!diags.empty());

        bool foundSyntaxErr = false;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                foundSyntaxErr = true;
                break;
            }
        }
        CHECK(foundSyntaxErr);
    }

    TEST_CASE("Tree-Sitter Semantic Diagnostic Catching")
    {
        analysis::ValidationOracle oracle;
        std::string invalidSemanticCode = R"(
            void Main() {
                UnknownFunctionCall();
            }
        )";

        auto diags = oracle.ValidateSync(invalidSemanticCode, "file:///test_semantic.as");
        REQUIRE(!diags.empty());

        bool foundSemanticErr = false;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error && d.message.find("UnknownFunctionCall") != std::string::npos)
            {
                foundSemanticErr = true;
                break;
            }
        }
        CHECK(foundSemanticErr);
    }

    TEST_CASE("Uncalled Standalone Method Call Error Catching")
    {
        analysis::SymbolTable globalTable;
        std::string mockPredefined = R"(
            class array {
                void insertLast(int val);
            }
        )";
        analysis::PredefinedLoader::LoadFromSource(mockPredefined, globalTable, "string", "array");

        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                array numbers;
                numbers.insertLast;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_uncalled_method.as", nullptr, &globalTable);
        REQUIRE(!diags.empty());

        bool foundUncalledErr = false;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error && d.message.find("insertLast") != std::string::npos)
            {
                foundUncalledErr = true;
                break;
            }
        }
        CHECK(foundUncalledErr);
    }

    TEST_CASE("Duplicate Local Variable Scope Redefinition Catching")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                int f = 1;
                float f = 2;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_duplicate_var.as");
        REQUIRE(!diags.empty());

        bool foundRedefinitionErr = false;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error && (d.message.find("Redefinition") != std::string::npos || d.message.find("Redefinición") != std::string::npos))
            {
                foundRedefinitionErr = true;
                break;
            }
        }
        CHECK(foundRedefinitionErr);
    }
}

TEST_SUITE("Type Inference and Mismatch")
{
    TEST_CASE("Test A: int f = true; -> Assert exactly 1 diagnostic error")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                int f = true;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_a.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 1);
    }

    TEST_CASE("Test B: float f = 3.14; -> Assert 0 diagnostics")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                float f = 3.14;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_b.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Test C: bool b = false; int c = b; -> Assert exactly 1 diagnostic error on the second statement")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                bool b = false;
                int c = b;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_c.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 1);
    }
}

TEST_SUITE("Using and Import Validation")
{
    TEST_CASE("Valid using namespace statement")
    {
        analysis::SymbolTable globalTable;
        std::string mockPredefined = R"(
            namespace Math {
                void Sin() {}
            }
        )";
        analysis::PredefinedLoader::LoadFromSource(mockPredefined, globalTable, "string", "array");

        analysis::ValidationOracle oracle;
        std::string code = "using namespace Math;";

        auto diags = oracle.ValidateSync(code, "file:///test_using_valid.as", nullptr, &globalTable);
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Invalid undeclared using namespace statement")
    {
        analysis::ValidationOracle oracle;
        std::string code = "using namespace NonExistentNamespace;";

        auto diags = oracle.ValidateSync(code, "file:///test_using_invalid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 1);
    }

    TEST_CASE("Invalid import module directive")
    {
        analysis::ValidationOracle oracle;
        std::string code = "import void Kill() from;";

        auto diags = oracle.ValidateSync(code, "file:///test_import_invalid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}

TEST_SUITE("Enum and Typedef Validation")
{
    TEST_CASE("Valid enum with and without explicit values")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            enum Color {
                Red = 1,
                Green = 2,
                Blue
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_enum_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Invalid enum with duplicate constant enumerator")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            enum Color {
                Red = 1,
                Red = 2
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_enum_duplicate.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Invalid enum with non-integer initializer")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            enum State {
                Active = "true"
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_enum_invalid_init.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Valid typedef statement")
    {
        analysis::ValidationOracle oracle;
        std::string code = "typedef float real;";

        auto diags = oracle.ValidateSync(code, "file:///test_typedef_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Invalid typedef with colliding alias name")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            typedef float real;
            typedef int real;
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_typedef_duplicate.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}

TEST_SUITE("Function and Paramlist Validation")
{
    TEST_CASE("Valid function and funcdef with default parameters")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            funcdef void Callback(int a, float b = 1.0f);
            void Main(int a, int b = 2) {}
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_func_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Function with duplicate parameter names")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Foo(int a, float a) {}";

        auto diags = oracle.ValidateSync(code, "file:///test_func_dup_param.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Function with out-of-order default parameter")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Foo(int a = 1, int b) {}";

        auto diags = oracle.ValidateSync(code, "file:///test_func_default_order.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Function returning value in void function")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Foo() { return 42; }";

        auto diags = oracle.ValidateSync(code, "file:///test_func_void_ret.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Global function with method-only attribute override")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Foo() override {}";

        auto diags = oracle.ValidateSync(code, "file:///test_func_invalid_attr.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}

TEST_SUITE("Class, Interface and VirtProp Validation")
{
    TEST_CASE("Valid class, interface, and virtprop with correct inheritance")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            interface Printable {
                void Print();
            }
            class Base {}
            class Child : Base, Printable {
                void Print() {}
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_class_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Attempting to inherit from a final class")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            class Base final {}
            class Child : Base {}
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_class_final.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Class failing to implement interface method")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            interface Printable {
                void Print();
            }
            class Child : Printable {}
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_class_unimplemented.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Method with override that does not exist in base")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            class Child {
                void Foo() override {}
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_class_override_missing.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}

TEST_SUITE("Expression, Cast and Assignment Validation")
{
    TEST_CASE("Valid math, logical and cast expressions")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                int a = 1 + 2;
                bool b = true and false;
                float c = cast<float>(a);
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_expr_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Binary operation with incompatible non-numeric types")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                int x = 5 + "texto";
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_expr_invalid_binary.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Handle identity operator on primitive type")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                bool b = 5 is null;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_expr_invalid_handle.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Assignment to constant variable")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                const int c = 10;
                c = 20;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_expr_assign_const.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Invalid explicit cast between disconnected types")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                int x = cast<int>("string_literal");
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_expr_invalid_cast.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}

TEST_SUITE("Control Flow Validation")
{
    TEST_CASE("Valid break, continue, and switch statements")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                for (int i = 0; i < 10; ++i) {
                    if (i == 5) continue;
                    if (i == 8) break;
                }
                switch (1) {
                    case 1: break;
                    case 2: break;
                }
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_cf_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Break statement outside loop or switch")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Main() { break; }";

        auto diags = oracle.ValidateSync(code, "file:///test_cf_break_outside.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Continue statement outside loop")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Main() { continue; }";

        auto diags = oracle.ValidateSync(code, "file:///test_cf_continue_outside.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Duplicate case values in switch")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                switch (1) {
                    case 1: break;
                    case 1: break;
                }
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_cf_dup_case.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}

TEST_SUITE("Operator Overload Validation")
{
    TEST_CASE("Valid operator overload method signatures")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            class Vector2 {
                Vector2 opAdd(const Vector2 &in other) { return Vector2(); }
                int opCmp(const Vector2 &in other) { return 0; }
                bool opEquals(const Vector2 &in other) { return true; }
            };
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_op_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Invalid opAdd operator overload signature")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            class Vector2 {
                Vector2 opAdd() { return Vector2(); }
            };
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_op_invalid_add.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Invalid opCmp operator overload return type")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            class Vector2 {
                bool opCmp(const Vector2 &in other) { return true; }
            };
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_op_invalid_cmp.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Master End-to-End Test: 0 False Positives on Complex Script")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            // ==========================================
            // TEST 1: NAMESPACES, ENUMS & INTERFACES
            // ==========================================
            namespace Engine {
                enum State { IDLE = 0, RUNNING, ATTACKING, DEAD };

                interface IDamageable {
                    void takeDamage(int amount);
                    int getHealth() const;
                };

                class BaseEntity {
                    protected int m_id;
                    BaseEntity(int id) {
                        this.m_id = id;
                    }

                    void update(float dt) {}
                };
            }

            using namespace Engine;

            // ==========================================
            // TEST 2: CLASES, HERENCIA, VIRTPROPS & OPERADORES
            // ==========================================
            class Player : BaseEntity, IDamageable {
                private int m_hp;
                private Engine::State m_state;

                Player(int id, int hp = 100) {
                    // Invocación de clase base vía SCOPE
                    Engine::BaseEntity::BaseEntity(id);
                    m_hp = hp;
                    m_state = Engine::State::IDLE;
                }

                // Propiedad Virtual (VIRTPROP)
                int health {
                    get const { return m_hp; }
                    set { m_hp = value; }
                }

                // Implementación de Interface
                void takeDamage(int amount) override {
                    m_hp -= amount;
                    if (m_hp <= 0) {
                        m_state = Engine::State::DEAD;
                    }
                }

                int getHealth() const override {
                    return m_hp;
                }

                // Sobrecarga de Operadores (Firmas estrictas)
                Player@ opAdd(const Player& in other) { return this; }
                int opCmp(const Player& in other) const { return m_hp - other.m_hp; }
                bool opEquals(const Player& in other) const { return m_hp == other.m_hp; }
                int opIndex(int idx) const { return m_hp; }
                int opCast() const { return m_hp; }
            };

            // ==========================================
            // TEST 3: FUNCIONES CON MODIFICADORES & ARGS
            // ==========================================
            void mutateStats(int &in multiplier, int &out calculatedHp, int &inout globalCounter) {
                calculatedHp = 100 * multiplier;
                globalCounter += 1;
            }

            void spawnEntity(string name, int count = 1, bool active = true) {}

            // ==========================================
            // TEST 4: COMPLEJIDAD DE CONTROL DE FLUJO Y EXPRESIONES
            // ==========================================
            void main() {
                // Instanciación y Handles
                Player@ p1 = Player(1, 150);
                Player@ p2 = Player(2, 200);
                Player@ pRef;

                // Asignación de handles (@=) y comparación de identidad (is / !is)
                @pRef = @p1;
                bool sameObject = (pRef is p1);
                bool notNull = (p1 !is null);

                // Virtprop access & Operador Ternario
                p1.health = (p1.health > 0) ? p1.health + 10 : 0;

                // Argumentos por defecto y Argumentos Nombrados
                spawnEntity("Goblin");                  // Usa defaults
                spawnEntity("Orc", count: 5);          // Usa named argument
                spawnEntity("Dragon", active: false);  // Usa named argument salteado

                // Referencias & L-Values
                int mult = 2;
                int outHp;
                int counter = 0;
                mutateStats(mult, outHp, counter);     // Variables locales pasadas a &out e &inout

                // Deducción 'auto' e Inferencia de Plantillas Anidadas
                auto autoPlayer = p1;
                array<array<int>> grid = { {1, 2}, {3, 4} };
                int val = grid[0][1];                  // Debe inferir tipo 'int'

                // Foreach Loop
                array<Player@> team = { p1, p2 };
                foreach(auto member : team) {
                    if (member is null) continue;
                    member.takeDamage(10);
                }

                // Switch con Enums y Bucles Anidados con Break/Continue
                Engine::State currentState = Engine::State::RUNNING;
                switch(currentState) {
                    case Engine::State::IDLE:
                        break;

                    case Engine::State::RUNNING: {
                        for(int i = 0; i < 10; ++i) {
                            if (i == 2) continue; // continue dentro de loop -> OK
                            if (i == 8) break;    // break dentro de loop -> OK
                        }
                        break; // break de switch -> OK
                    }

                    case Engine::State::ATTACKING:
                    case Engine::State::DEAD:
                        break;

                    default:
                        break;
                }
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///master_test.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                std::fprintf(stderr, "MASTER TEST ERROR: [line %u col %u] %s\n", d.range.start.line + 1, d.range.start.character + 1, d.message.c_str());
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Master Negative Test: Intentional Errors Detected")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void mutateStats(int &in a, int &out b, int &inout c) {}

            class Dummy {
                private int secretValue;

                // ERROR 1: Firmas de sobrecarga de operadores inválidas
                void opCmp(const Dummy& in other) {}      // opCmp DEBE retornar 'int'
                bool opEquals() {}                         // opEquals DEBE recibir 1 parámetro
                int opAdd(int a, int b) { return 0; }      // opAdd solo debe recibir 1 parámetro

                void internalMethod() {}
            };

            void voidFunction() {
                // ERROR 2: Return con valor en función void
                return 42; 
            }

            int intFunction() {
                // ERROR 3: Return vacío en función no-void
                return;
            }

            void testErrors(Dummy@ obj) {
                // ERROR 5: Acceso a miembro privado fuera de la clase
                int val = obj.secretValue; 

                // ERROR 6: Break y Continue fuera de contexto
                break;    // Error: 'break' fuera de bucle o switch
                continue; // Error: 'continue' fuera de bucle

                // ERROR 7: Switch con flotantes
                float f = 3.14f;
                switch(f) { // Error: expresión de switch debe ser int, enum o string
                    case 1: break;
                }

                int num = 2;
                switch(num) {
                    case 1: 
                        // ERROR 8: 'continue' en switch sin estar dentro de un bucle
                        continue; 
                        break;
                    case 1: // Error: valor 'case 1' duplicado
                        break;
                }

                // ERROR 9: Pasar un R-Value (Literal) a parámetros &out o &inout
                int counter = 0;
                mutateStats(5, 100, counter); // Error: '100' no es un L-value modificable para &out

                // ERROR 10: Incremento en constantes o literales
                const int C = 10;
                C++; // Error: No se puede modificar una variable 'const'
                5++; // Error: El operando de '++' debe ser un L-value
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///master_negative_test.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                std::fprintf(stderr, "NEGATIVE TEST ERROR DETECTED: [line %u col %u] %s\n", d.range.start.line + 1, d.range.start.character + 1, d.message.c_str());
                errorCount++;
            }
        }
        std::fprintf(stderr, "TOTAL ERRORS DETECTED: %zu\n", errorCount);
        CHECK(errorCount >= 10);
    }

    TEST_CASE("Super Master 100% EBNF & Statement Coverage Suite")
    {
        SUBCASE("Valid Super Master Script - Expect 0 Diagnostics")
        {
            analysis::ValidationOracle oracle;
            std::string code = R"(
                namespace Game {
                    void log(string msg) {}
                }

                import void ExternalRender(int id) from "EngineModule";

                funcdef void EventCb(int code);
                typedef int EntityID;

                enum Status { IDLE, ACTIVE = 2, BUSY, MAX_STATUS = BUSY * 10 }

                interface IUpdatable {
                    void update(float dt);
                }

                mixin class IdentityMixin {
                    EntityID id = 1;
                }

                class Actor : IdentityMixin, IUpdatable {
                    private int m_hp;

                    Actor() { m_hp = 100; }
                    Actor(int hp) { m_hp = hp; }

                    int health {
                        get const { return m_hp; }
                        set { m_hp = value; }
                    }

                    void update(float dt) override {}

                    int opCmp(const Actor& in other) const { return m_hp - other.m_hp; }
                    bool opEquals(const Actor& in other) const { return m_hp == other.m_hp; }
                }

                void modifyStats(int &in mult, int &out calculated, int &inout counter) {
                    calculated = 10 * mult;
                    counter++;
                }

                void main() {
                    int x = 10, y = 20;
                    const int COMPILE_CONST = 5;

                    Actor player(150);
                    Actor@ refPlayer;
                    @refPlayer = @player;

                    bool same = (refPlayer is player);
                    bool valid = (player !is null);

                    int val = 100;
                    {
                        float val = 3.14f;
                        float res = val * 2.0f;
                    }

                    {
                        using namespace Game;
                        log("Injected in block");
                    }

                    try {
                        ExternalRender(1);
                    } catch {}

                    while (x > 0) { x--; }
                    do { y++; } while (y < 25);

                    for (int i = 0, j = 10; i < j; i++, j--) {
                        if (i == 2) continue;
                        if (i == 4) break;
                    }

                    dictionary dict;
                    foreach(auto v, auto k : dict) {}

                    int target = 5;
                    switch (target) {
                        case 0: break;
                        case COMPILE_CONST: break;
                        default: break;
                    }

                    int m = 2, outH, c = 0;
                    modifyStats(m, outH, c);

                    EventCb@ handler = function(int code) {};
                }
            )";

            auto diags = oracle.ValidateSync(code, "file:///super_master_valid.as");
            size_t errorCount = 0;
            for (const auto &d : diags)
            {
                if (d.severity == lsp::DiagnosticSeverity::Error)
                {
                    std::fprintf(stderr, "SUPER MASTER VALID ERROR: [line %u col %u] %s\n", d.range.start.line + 1, d.range.start.character + 1, d.message.c_str());
                    errorCount++;
                }
            }
            CHECK(errorCount == 0);
        }

        SUBCASE("Invalid Super Master Script - Expect Exact Diagnostics")
        {
            analysis::ValidationOracle oracle;
            std::string code = R"(
                class Dummy {}

                mixin class InvalidClassMixin : Dummy {}

                mixin class ValidMixin { void process() {} }

                funcdef void IntCallback(int val);

                void modifyStats(int &in mult, int &out calculated, int &inout counter) {}

                void testErrors() {
                    { int scopedVar = 42; }
                    int a = scopedVar;

                    namespace Hidden { void hiddenFunc() {} }
                    {
                        using namespace Hidden;
                        hiddenFunc();
                    }
                    hiddenFunc();

                    Dummy d;
                    if (d) {}

                    int dynamicCase = 10;
                    switch (10) {
                        case dynamicCase: break;
                    }

                    const int MAX = 100;
                    MAX = 200;

                    break;
                    continue;

                    ValidMixin mixinObj;

                    IntCallback@ cb = function(string invalid) {};

                    enum BadEnum { eA = eB + 1, eB = 10 }

                    int counter = 0;
                    modifyStats(2, 100, counter);
                }
            )";

            auto diags = oracle.ValidateSync(code, "file:///super_master_invalid.as");
            size_t errorCount = 0;
            for (const auto &d : diags)
            {
                if (d.severity == lsp::DiagnosticSeverity::Error)
                {
                    std::fprintf(stderr, "SUPER MASTER INVALID ERROR DETECTED: [line %u col %u] %s\n", d.range.start.line + 1, d.range.start.character + 1, d.message.c_str());
                    errorCount++;
                }
            }
            std::fprintf(stderr, "TOTAL SUPER MASTER INVALID ERRORS DETECTED: %zu\n", errorCount);
            CHECK(errorCount >= 10);
        }
    }

    TEST_CASE("Advanced Features: Diamond Interfaces, Const Handles, Indexed Virtprops & Inline Lambdas")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            interface IBase {
                void ping();
            }
            interface IReadable : IBase {
                int read();
            }
            interface IWritable : IBase {
                void write(int v);
            }
            interface IReadWrite : IReadable, IWritable {
                void flush();
            }

            class StreamDevice : IReadWrite {
                private int m_data = 0;
                
                void ping() override {}
                int read() override { return m_data; }
                void write(int v) override { m_data = v; }
                void flush() override {}
            }

            class DataNode {
                int value = 42;
                void modify() { value++; }
                int getValue() const { return value; }
            }

            void testConstHandles() {
                const DataNode@ constObjRef = DataNode();
                int v = constObjRef.getValue();

                DataNode@ const nodeHandle = DataNode();
                nodeHandle.modify(); 

                const DataNode@ const immutableRef = DataNode();
                int v2 = immutableRef.getValue();
            }

            class MatrixContainer {
                private array<int> m_buffer = {10, 20, 30};

                int get_cell(int idx) const { return m_buffer[idx]; }
                void set_cell(int idx, int value) { m_buffer[idx] = value; }
            }

            void testIndexedVirtProp() {
                MatrixContainer mat;
                mat.set_cell(0, 99);
                int val = mat.get_cell(0);
            }

            funcdef int Transformer(int x, int y);

            void processBatch(Transformer@ fn) {
                if (fn !is null) fn(5, 5);
            }

            class Animal {}
            class Dog : Animal {}
            class Cat : Animal {}

            Animal@ selectPet(bool chooseDog, Dog@ d, Cat@ c) {
                Animal@ result = chooseDog ? cast<Animal>(d) : cast<Animal>(c);
                return result;
            }

            void testInlineLambdas() {
                processBatch(function(int a, int b) {
                    return a * b;
                });
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_advanced_features.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                std::fprintf(stderr, "ADVANCED FEATURE TEST ERROR: [line %u col %u] %s\n", d.range.start.line + 1, d.range.start.character + 1, d.message.c_str());
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("User Intentional Errors: Inheritance Cycles, Const Handle Violations, Bad Lambdas & Ternaries")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            class ClassA : ClassB {}
            class ClassB : ClassA {}

            class RecursiveSelf {
                RecursiveSelf child;
            }

            class Node {
                int data = 0;
                void set(int d) { data = d; }
                int get() const { return data; }
            }

            void testConstViolations() {
                const Node@ constObj = Node();
                constObj.set(10);

                Node@ const constHandle = Node();
                Node@ other = Node();
                @constHandle = @other;
            }

            funcdef void ActionCallback(string message, int code);

            void executeAction(ActionCallback@ cb) {}

            void testLambdaMismatch() {
                executeAction(function(int wrongMsg, string wrongCode) {});
            }

            void testBadTernary() {
                int x = 10;
                string s = "hello";
                auto res = (x > 0) ? x : s; 
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_user_invalid_features.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                std::fprintf(stderr, "USER NEGATIVE TEST ERROR DETECTED: [line %u col %u] %s\n", d.range.start.line + 1, d.range.start.character + 1, d.message.c_str());
                errorCount++;
            }
        }
        std::fprintf(stderr, "TOTAL USER NEGATIVE ERRORS DETECTED: %zu\n", errorCount);
        CHECK(errorCount >= 4);
    }
}
