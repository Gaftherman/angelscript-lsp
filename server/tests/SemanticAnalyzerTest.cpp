#include <doctest/doctest.h>
#include <iostream>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("SemanticAnalyzer - Level 1: Missing Function Body")
{
    std::string sourceCode = "void TestFunc();\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-missing-body");
    CHECK(diagnostics[0].severity == DiagnosticSeverity::Error);
}

TEST_CASE("SemanticAnalyzer - Level 1: Mixin Final and Abstract")
{
    std::string sourceCode = "mixin final class MixinA {}\nmixin abstract class MixinB {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-mixin-final");
    CHECK(diagnostics[1].code == "as-err-mixin-abstract");
}

TEST_CASE("SemanticAnalyzer - Level 2: Handle on Primitive")
{
    std::string sourceCode = "int@ globalVar;\nvoid Func(float@ p) {}\nint@ RetFunc() { return 0; }\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 3);
    CHECK(diagnostics[0].code == "as-err-handle-on-primitive");
    CHECK(diagnostics[1].code == "as-err-handle-on-primitive");
    CHECK(diagnostics[2].code == "as-err-handle-on-primitive");
}

TEST_CASE("SemanticAnalyzer - Level 2: Void Variable and Out Default Parameter")
{
    std::string sourceCode = "void x;\nvoid Func(int &out val = 5) {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-void-variable");
    CHECK(diagnostics[1].code == "as-err-out-param-default");
}

TEST_CASE("SemanticAnalyzer - Level 3: Duplicate Symbol Declarations")
{
    std::string sourceCode = "int gVar;\nint gVar;\nvoid Action(int a) {}\nvoid Action(int b) {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-duplicate-symbol");
    CHECK(diagnostics[1].code == "as-err-duplicate-symbol");
}

TEST_CASE("SemanticAnalyzer - Level 3: Duplicate Parameter Names and Global Shadowing")
{
    std::string sourceCode = "int globalVal;\nvoid Func(int a, float a) {}\nvoid ShadowFunc(int globalVal) {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-duplicate-param");
    CHECK(diagnostics[0].severity == DiagnosticSeverity::Error);
    CHECK(diagnostics[1].code == "as-warn-shadow-global");
    CHECK(diagnostics[1].severity == DiagnosticSeverity::Warning);
}

TEST_CASE("SemanticAnalyzer - Level 4: Inheritance and Base Validation")
{
    std::string sourceCode = "final class FinalBase {}\nclass SubClass : FinalBase, MissingBase {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-inherit-final");
    CHECK(diagnostics[1].code == "as-err-base-not-found");
}

TEST_CASE("SemanticAnalyzer - Level 4: Multiple Class Inheritance and Unresolved Typedef")
{
    std::string sourceCode = "class BaseA {}\nclass BaseB {}\nclass Derived : BaseA, BaseB {}\ntypedef UnknownType CustomType;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-multi-class-inherit");
    CHECK(diagnostics[1].code == "as-err-typedef-unresolved");
}

TEST_CASE("SemanticAnalyzer - Level 4: Funcdef Variable Without Handle")
{
    std::string sourceCode = "funcdef void Callback();\nCallback cb;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-funcdef-not-handle");
}

TEST_CASE("SemanticAnalyzer - Level 5: Circular Inheritance Detection")
{
    std::string sourceCode = "class NodeA : NodeB {}\nclass NodeB : NodeA {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() >= 1);
    bool foundCircular = false;
    for (const auto &diag : diagnostics)
    {
        if (diag.code == "as-err-circular-inherit")
        {
            foundCircular = true;
            break;
        }
    }
    CHECK(foundCircular == true);
}

TEST_CASE("SemanticAnalyzer - Array Handle Validation")
{
    std::string sourceCode = "int[]@ arrayIntAnotherHandle;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Property Void Type Validation")
{
    std::string sourceCode = "void needBody {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-void-variable");
}

TEST_CASE("SemanticAnalyzer - Interface methods do not generate missing-body error")
{
    // interface_method nodes never have a body field in the AST.
    // Before SC-06/SA-06, every interface method generated as-err-missing-body.
    std::string sourceCode =
        "interface IAnimal\n"
        "{\n"
        "    void Speak();\n"
        "    int GetAge();\n"
        "    float GetWeight(int unit);\n"
        "}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Regular function still requires body")
{
    // Ensure the isInterfaceMethod guard does not suppress errors for regular
    // non-interface functions that are missing their body.
    std::string sourceCode =
        "interface IValid { void Speak(); }\n"
        "void MissingBody();\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-missing-body");
    CHECK(diagnostics[0].message.find("MissingBody") != std::string::npos);
}

TEST_CASE("SemanticAnalyzer - SA-07: Unresolved type error")
{
    std::string sourceCode =
        "UnknownType g_var;\n"
        "void Func(MissingType param) {}\n"
        "UndefinedReturn GetThing() { return 0; }\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 3);
    CHECK(diagnostics[0].code == "as-err-unresolved-type");
    CHECK(diagnostics[1].code == "as-err-unresolved-type");
    CHECK(diagnostics[2].code == "as-err-unresolved-type");
}

TEST_CASE("SemanticAnalyzer - SA-08: Const out parameter error")
{
    std::string sourceCode = "void Func(const int &out x) {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-const-out-param");
}

TEST_CASE("SemanticAnalyzer - SA-09: Mixin cannot be base of non-mixin class")
{
    std::string sourceCode =
        "mixin class MyMixin {}\n"
        "class Derived : MyMixin {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-mixin-as-base");
}

TEST_CASE("SemanticAnalyzer - SA-07: Known types do NOT generate unresolved-type (false positive guard)")
{
    // typedef'd, enum, and class types that exist in the same file must NOT be flagged.
    std::string sourceCode =
        "typedef int MyInt;\n"
        "enum Color { Red = 1 }\n"
        "class Player {}\n"
        "MyInt x;\n"
        "Color c;\n"
        "Player p;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - SA-07: Funcdef unresolved return type")
{
    // funcdef return type should also be validated for resolution.
    std::string sourceCode = "funcdef UnknownType BadCallback(int x);\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(!diagnostics.empty());
    bool found = false;
    for (const auto &d : diagnostics)
        if (d.code == "as-err-unresolved-type") { found = true; break; }
    CHECK(found);
}

TEST_CASE("SemanticAnalyzer - SA-08: const in param is valid (no error)")
{
    // const &in is perfectly valid in AngelScript — only const &out is the error.
    std::string sourceCode = "void Func(const int &in x) {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - SA-10: Interface method implementation check")
{
    std::string sourceCode =
        "interface IAnimal\n"
        "{\n"
        "    void Speak();\n"
        "    int GetAge();\n"
        "}\n"
        "class Dog : IAnimal\n"
        "{\n"
        "    void Speak() {}\n"
        "}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-interface-impl-missing");
    CHECK(diagnostics[0].message.find("GetAge") != std::string::npos);
}

TEST_CASE("Grammar - function@ as parameter type parses without error")
{
    std::string sourceCode =
        "funcdef void function();\n"
        "void test_function(function@ f) { f(); }\n";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto syntaxDiags = collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    bool hasSyntaxError = false;
    for (const auto &d : syntaxDiags)
        if (d.code == "as-syntax-error" || d.code == "as-syntax-error-missing")
            hasSyntaxError = true;
    CHECK(!hasSyntaxError);
}

TEST_CASE("Grammar - function@ variable with lambda rhs parses correctly")
{
    std::string sourceCode =
        "funcdef void function();\n"
        "function@ myFunc = function() {};\n";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto syntaxDiags = collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    bool hasSyntaxError = false;
    for (const auto &d : syntaxDiags)
        if (d.code == "as-syntax-error" || d.code == "as-syntax-error-missing")
            hasSyntaxError = true;
    CHECK(!hasSyntaxError);
}

TEST_CASE("SemanticAnalyzer - class name matching modifier allowed per native spec (final)")
{
    std::string sourceCode = "final class final {}\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, "file:///test.as", "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - class name matching modifier allowed per native spec (abstract)")
{
    std::string sourceCode = "abstract class abstract {}\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, "file:///test.as", "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - reserved keyword as function name")
{
    std::string sourceCode = "int int() {}\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, "file:///test.as", "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    bool found = false;
    for (const auto &d : diagnostics)
        if (d.code == "as-err-reserved-keyword-name") { found = true; break; }
    CHECK(found);
}

TEST_CASE("SemanticAnalyzer - reserved keyword as funcdef name")
{
    std::string sourceCode = "funcdef void funcdef();\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, "file:///test.as", "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    bool found = false;
    for (const auto &d : diagnostics)
        if (d.code == "as-err-reserved-keyword-name") { found = true; break; }
    CHECK(found);
}

TEST_CASE("SemanticAnalyzer - reserved keyword as interface name")
{
    std::string sourceCode = "interface interface {}\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, "file:///test.as", "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    bool found = false;
    for (const auto &d : diagnostics)
        if (d.code == "as-err-reserved-keyword-name") { found = true; break; }
    CHECK(found);
}

TEST_CASE("SemanticAnalyzer - reserved keyword as class name")
{
    std::string sourceCode = "class class {}\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, "file:///test.as", "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    bool found = false;
    for (const auto &d : diagnostics)
        if (d.code == "as-err-reserved-keyword-name") { found = true; break; }
    CHECK(found);
}

TEST_CASE("SemanticAnalyzer - cross type name conflict function vs class")
{
    std::string sourceCode = "class SomeHandle {}\nSomeHandle SomeHandle() {}\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, "file:///test.as", "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    bool found = false;
    for (const auto &d : diagnostics)
        if (d.code == "as-err-name-conflict") { found = true; break; }
    CHECK(found);
}

TEST_CASE("Batch_105_Structural_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {1, "Namespaces", "namespace N { void f(){} }"},
        {2, "Namespaces", "namespace N1 { namespace N2 { class C{} } }"},
        {3, "Namespaces", "namespace 123Bad {}"},
        {4, "Namespaces", "namespace class {}"},
        {5, "Namespaces", "namespace N { int x = 10; }"},
        {6, "Namespaces", "namespace N::M { class C{} }"},
        {7, "Namespaces", "namespace N { funcdef void CB(); }"},
        {8, "Namespaces", "namespace N { typedef float Real; }"},
        {9, "Namespaces", "namespace N { int get_p() { return 0; } }"},
        {10, "Namespaces", "namespace N { class C { void m(){} } }"},
        {11, "Namespaces", "namespace { void f(){} }"},
        {12, "Namespaces", "namespace N {"},

        {13, "Functions", "void f(int a, float b = 1.0) {}"},
        {14, "Functions", "void f(const int &in a) {}"},
        {15, "Functions", "void f(int &out a) {}"},
        {16, "Functions", "void f(int &inout a) {}"},
        {17, "Functions", "void f(int@ handle) {}"},
        {18, "Functions", "void f(int a,) {}"},
        {19, "Functions", "fucntion void f() {}"},
        {20, "Functions", "vodi f() {}"},
        {21, "Functions", "const void f() {}"},
        {22, "Functions", "void f() const {}"},
        {23, "Functions", "void f() override {}"},
        {24, "Functions", "void f() final {}"},
        {25, "Functions", "void f() external;"},
        {26, "Functions", "int f(int a, int b) { return a + b; }"},
        {27, "Functions", "void f(int a = 1, float b) {}"},
        {28, "Functions", "void f(const &in a) {}"},

        {29, "Variables", "int g_var = 100;"},
        {30, "Variables", "const float g_pi = 3.14159f;"},
        {31, "Variables", "private int g_priv = 1;"},
        {32, "Variables", "int a, b = 2, c;"},
        {33, "Variables", "int a, ;"},
        {34, "Variables", "int a = ;"},
        {35, "Variables", "protected string g_s;"},
        {36, "Variables", "auto g_auto = 42;"},
        {37, "Variables", "int[] arr;"},
        {38, "Variables", "int@ obj;"},
        {39, "Variables", "void v_var;"},
        {40, "Variables", "int a = 1, float b = 2;"},

        {41, "Classes", "class MyClass {}"},
        {42, "Classes", "class Base {} class Derived : Base {}"},
        {43, "Classes", "interface IFace {} class Impl : IFace { void m(){} }"},
        {44, "Classes", "class Multi : Base1, Base2 {}"},
        {45, "Classes", "final class FinalClass {}"},
        {46, "Classes", "abstract class AbstractClass {}"},
        {47, "Classes", "class C { private: int x; protected: float y; public: void f(){} }"},
        {48, "Classes", "class C { ~C() {} }"},
        {49, "Classes", "class C {};;"},
        {50, "Classes", "clas C {}"},
        {51, "Classes", "final class final {}"},
        {52, "Classes", "abstract class abstract {}"},
        {53, "Classes", "class C { int a; float b; void m() {} }"},
        {54, "Classes", "class C : NonExistent {}"},
        {55, "Classes", "class C { C() {} }"},
        {56, "Classes", "class C { void f() const {} }"},
        {57, "Classes", "class C { void f() override {} }"},
        {58, "Classes", "class C { void f() final {} }"},

        {59, "Interfaces", "interface ITest { void DoWork(); }"},
        {60, "Interfaces", "interface ITest { int x; }"},
        {61, "Interfaces", "interface ITest { void DoWork() {} }"},
        {62, "Interfaces", "interface IChild : IParent {}"},
        {63, "Interfaces", "intreface ITest {}"},
        {64, "Interfaces", "interface ITest { void f(int a, float b); }"},
        {65, "Interfaces", "interface ITest { ITest() {} }"},
        {66, "Interfaces", "interface ITest { ~ITest() {} }"},
        {67, "Interfaces", "interface ITest { private: void f(); }"},
        {68, "Interfaces", "interface ITest { int get_prop() const; }"},
        {69, "Interfaces", "interface ITest { void set_prop(int); }"},
        {70, "Interfaces", "interface I1, I2 {}"},

        {71, "Mixins", "mixin class Mix1 { void mixFunc() {} }"},
        {72, "Mixins", "mixin class Mix1; class C { mixin Mix1; }"},
        {73, "Mixins", "class C { mixin NonExistent; }"},
        {74, "Mixins", "mixin class Mix1 : Base {}"},
        {75, "Mixins", "final mixin class Mix1 {}"},
        {76, "Mixins", "abstract mixin class Mix1 {}"},
        {77, "Mixins", "mixin class Mix1 { int x; }"},
        {78, "Mixins", "mixn class Mix1 {}"},
        {79, "Mixins", "class C { mixin; }"},
        {80, "Mixins", "mixin Mix1;"},

        {81, "TypedefFuncdef", "typedef int MyInt;"},
        {82, "TypedefFuncdef", "typedef float MyFloat;"},
        {83, "TypedefFuncdef", "typedef NonExistent MyType;"},
        {84, "TypedefFuncdef", "funcdef void Callback(int a, float b);"},
        {85, "TypedefFuncdef", "funcdef int MathOp(int, int);"},
        {86, "TypedefFuncdef", "funcdef void BadFunc(int a,);"},
        {87, "TypedefFuncdef", "typdf int MyInt;"},
        {88, "TypedefFuncdef", "fncdef void CB();"},
        {89, "TypedefFuncdef", "funcdef void CB(int a = 1);"},
        {90, "TypedefFuncdef", "funcdef const void CB();"},

        {91, "Properties", "class PropClass { int get_val() const { return 0; } void set_val(int v) {} }"},
        {92, "Properties", "int get_globalVal() { return 10; }"},
        {93, "Properties", "class BadProp { int get_val(int param) { return 0; } }"},
        {94, "Properties", "class BadSet { void set_val() {} }"},
        {95, "Properties", "void set_globalVal(float x) {}"},
        {96, "Properties", "class C { int prop { get const; set; } }"},

        {97, "LexicalStructural", "// comment line"},
        {98, "LexicalStructural", "/* multi line comment */"},
        {99, "LexicalStructural", "class Unclosed {"},
        {100, "LexicalStructural", "namespace UnclosedNS {"},
        {101, "LexicalStructural", "int int() {}"},
        {102, "LexicalStructural", "interface interface {}"},
        {103, "LexicalStructural", "class class {}"},
        {104, "LexicalStructural", "funcdef funcdef funcdef();"},
        {105, "LexicalStructural", "class SomeHandle {} SomeHandle SomeHandle() {}"}
    };

    angel_lsp::i18n::I18n i18n("en");
    std::cout << "\n=== LSP_VALIDATOR_BATCH_OUTPUT_START ===\n";

    for (const auto &tc : cases)
    {
        std::string fileUri = "file:///test_" + std::to_string(tc.id) + ".as";
        SymbolTable table;
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);

        auto syntaxDiags = collector.CollectSymbols(fileUri, tc.code, parser, table);

        SemanticAnalyzer analyzer;
        SemanticAnalysisRequest req{table, fileUri, "", &i18n};
        auto semanticDiags = analyzer.Analyze(req);

        std::vector<Diagnostic> allDiags = syntaxDiags;
        allDiags.insert(allDiags.end(), semanticDiags.begin(), semanticDiags.end());

        bool rejected = !allDiags.empty();
        std::string firstErr = "";
        if (rejected)
        {
            const auto &d = allDiags[0];
            firstErr = "L" + std::to_string(d.range.start.line + 1) + ":C" + std::to_string(d.range.start.character + 1) + " - [" + d.code + "] " + d.message;
        }

        std::cout << "ID:" << tc.id << "|"
                  << "STATUS:" << (rejected ? "RECHAZADO" : "ACEPTADO") << "|"
                  << "ERR:" << firstErr << "\n";
    }

    std::cout << "=== LSP_VALIDATOR_BATCH_OUTPUT_END ===\n";
}






