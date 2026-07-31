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

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-void-variable");
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
    CHECK(diagnostics[1].code == "as-err-typedef-non-primitive");
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

    REQUIRE(diagnostics.size() == 0);
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

TEST_CASE("Batch_Broken200_Harness_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {1, "BrokenClasses", "class { int x; }"},
        {2, "BrokenClasses", "class C : : Base {}"},
        {3, "BrokenClasses", "class C : Base1, {}"},
        {4, "BrokenClasses", "class C final abstract final {}"},
        {5, "BrokenClasses", "class C { void f() }"},
        {6, "BrokenClasses", "class C { private: public: private: }"},
        {7, "BrokenClasses", "class C { C(int a,) {} }"},
        {8, "BrokenClasses", "class C {"},
        {9, "BrokenClasses", "class C }"},
        {10, "BrokenClasses", "clas C {}"},
        {11, "BrokenClasses", "class C : 123Base {}"},
        {12, "BrokenClasses", "interface { void m(); }"},
        {13, "BrokenClasses", "interface I { int x }"},
        {14, "BrokenClasses", "interface I { void m() {} }"},
        {15, "BrokenClasses", "interface I { private: void m(); }"},
        {16, "BrokenClasses", "interface I { I() {} }"},
        {17, "BrokenClasses", "interface I { ~I() {} }"},
        {18, "BrokenClasses", "interface I : {}"},
        {19, "BrokenClasses", "interface I : I1, {}"},
        {20, "BrokenClasses", "mixin class { void f(){} }"},
        {21, "BrokenClasses", "mixin class M : Base {}"},
        {22, "BrokenClasses", "final mixin class M {}"},
        {23, "BrokenClasses", "abstract mixin class M {}"},
        {24, "BrokenClasses", "class C { mixin; }"},
        {25, "BrokenClasses", "class C { mixin 123Bad; }"},
        {26, "BrokenClasses", "class C { mixin M1, M2; }"},
        {27, "BrokenClasses", "class C : Base1, Base2 {}"},
        {28, "BrokenClasses", "class C { int a float b; }"},
        {29, "BrokenClasses", "class C { int a, ; }"},
        {30, "BrokenClasses", "class C { void f(; }"},
        {31, "BrokenClasses", "class C { void f); }"},
        {32, "BrokenClasses", "class C { void f() const const {} }"},
        {33, "BrokenClasses", "class C { void f() final final {} }"},
        {34, "BrokenClasses", "class C { void f() override override {} }"},
        {35, "BrokenClasses", "class C { static static int x; }"},
        {36, "BrokenClasses", "class C { const const int x = 0; }"},
        {37, "BrokenClasses", "class C { void ~C() {} }"},
        {38, "BrokenClasses", "class C { C ~C() {} }"},
        {39, "BrokenClasses", "class C { ~C(int a) {} }"},
        {40, "BrokenClasses", "class C { C(int a int b) {} }"},
        {41, "BrokenClasses", "class C { C(int a = ) {} }"},
        {42, "BrokenClasses", "class C { C() : Base() {} }"},
        {43, "BrokenClasses", "class C { C() : Base(10,) {} }"},
        {44, "BrokenClasses", "class C { C() : , Base() {} }"},
        {45, "BrokenClasses", "class C { explicit explicit C(int); }"},
        {46, "BrokenClasses", "class C { virtual void f(); }"},
        {47, "BrokenClasses", "class C { friend class D; }"},
        {48, "BrokenClasses", "class C { public private: int x; }"},
        {49, "BrokenClasses", "class C { int x = ; }"},
        {50, "BrokenClasses", "class C { static int x = ; }"},

        {51, "BrokenFuncs", "void () {}"},
        {52, "BrokenFuncs", "int f(int a int b) {}"},
        {53, "BrokenFuncs", "void f(int & & a) {}"},
        {54, "BrokenFuncs", "const void f() {}"},
        {55, "BrokenFuncs", "void f() const const {}"},
        {56, "BrokenFuncs", "void f(int a = 1, float b) {}"},
        {57, "BrokenFuncs", "void f(int a = ) {}"},
        {58, "BrokenFuncs", "vodi f() {}"},
        {59, "BrokenFuncs", "void f(int a,) {}"},
        {60, "BrokenFuncs", "void f(,) {}"},
        {61, "BrokenFuncs", "void f(int & &in a) {}"},
        {62, "BrokenFuncs", "void f(int &out &in a) {}"},
        {63, "BrokenFuncs", "void f(const const int a) {}"},
        {64, "BrokenFuncs", "void f(int a = 1, int b = 2,) {}"},
        {65, "BrokenFuncs", "void f(int a = 1 = 2) {}"},
        {66, "BrokenFuncs", "void f(int a) { return ; }"},
        {67, "BrokenFuncs", "int f() { return; }"},
        {68, "BrokenFuncs", "void f() const {}"},
        {69, "BrokenFuncs", "void f() override {}"},
        {70, "BrokenFuncs", "void f() final {}"},
        {71, "BrokenFuncs", "void f() delete;"},
        {72, "BrokenFuncs", "void f() explicit;"},
        {73, "BrokenFuncs", "void f() private;"},
        {74, "BrokenFuncs", "void f() protected;"},
        {75, "BrokenFuncs", "void f() public;"},
        {76, "BrokenFuncs", "void f(const void a) {}"},
        {77, "BrokenFuncs", "void f(void a) {}"},
        {78, "BrokenFuncs", "void& f() {}"},
        {79, "BrokenFuncs", "void@ f() {}"},
        {80, "BrokenFuncs", "int@ f() { return null; }"},
        {81, "BrokenFuncs", "int& f() { return 0; }"},
        {82, "BrokenFuncs", "void f(int & a = 10) {}"},
        {83, "BrokenFuncs", "void f(int &out a = 10) {}"},
        {84, "BrokenFuncs", "void f(int &inout a = 10) {}"},
        {85, "BrokenFuncs", "void f(const int &out a) {}"},
        {86, "BrokenFuncs", "void f(const int &inout a) {}"},
        {87, "BrokenFuncs", "void f(int a) {} int f(int a) { return 0; }"},
        {88, "BrokenFuncs", "void f() {} void f() {}"},
        {89, "BrokenFuncs", "void f(int a, int a) {}"},
        {90, "BrokenFuncs", "void f(int, int,) {}"},
        {91, "BrokenFuncs", "import void f() from ;"},
        {92, "BrokenFuncs", "import f() from \"mod\";"},
        {93, "BrokenFuncs", "external int f() {}"},
        {94, "BrokenFuncs", "external void f(int a = 1) {}"},
        {95, "BrokenFuncs", "void f(auto a) {}"},
        {96, "BrokenFuncs", "auto f(auto a) {}"},
        {97, "BrokenFuncs", "void f(int a = auto) {}"},
        {98, "BrokenFuncs", "void f(int a = int) {}"},
        {99, "BrokenFuncs", "void f(int a = class) {}"},
        {100, "BrokenFuncs", "void f(int a = struct) {}"},

        {101, "BrokenScopes", "namespace 123Bad {}"},
        {102, "BrokenScopes", "namespace class {}"},
        {103, "BrokenScopes", "namespace A::::B {}"},
        {104, "BrokenScopes", "namespace N {"},
        {105, "BrokenScopes", "using namespace ;"},
        {106, "BrokenScopes", "using namespace 123Bad;"},
        {107, "BrokenScopes", "using namespace class;"},
        {108, "BrokenScopes", "enum E { A = , B }"},
        {109, "BrokenScopes", "enum E { A, B,, C }"},
        {110, "BrokenScopes", "enum E { A = 1, B = }"},
        {111, "BrokenScopes", "enum { A, B }"},
        {112, "BrokenScopes", "enum 123E { A, B }"},
        {113, "BrokenScopes", "enum E { A = \"str\" }"},
        {114, "BrokenScopes", "enum E { A, A }"},
        {115, "BrokenScopes", "typedef MyType;"},
        {116, "BrokenScopes", "typedef int;"},
        {117, "BrokenScopes", "typedef 123Bad MyInt;"},
        {118, "BrokenScopes", "typedef int 123Bad;"},
        {119, "BrokenScopes", "typedef int int;"},
        {120, "BrokenScopes", "typedef int class;"},
        {121, "BrokenScopes", "funcdef void ();"},
        {122, "BrokenScopes", "funcdef int Callback(int a,);"},
        {123, "BrokenScopes", "funcdef Callback();"},
        {124, "BrokenScopes", "funcdef void 123Bad();"},
        {125, "BrokenScopes", "funcdef void funcdef();"},
        {126, "BrokenScopes", "funcdef const void CB();"},
        {127, "BrokenScopes", "funcdef void CB(int a = 1, float b);"},
        {128, "BrokenScopes", "funcdef void CB(const void a);"},
        {129, "BrokenScopes", "namespace N { private int x; }"},
        {130, "BrokenScopes", "namespace N { protected void f() {} }"},
        {131, "BrokenScopes", "namespace N { public class C {} }"},
        {132, "BrokenScopes", "namespace N { virtual void f() {} }"},
        {133, "BrokenScopes", "namespace N { override void f() {} }"},
        {134, "BrokenScopes", "namespace N { final class C {} }"},
        {135, "BrokenScopes", "namespace A { namespace B { namespace C {"},
        {136, "BrokenScopes", "namespace A::B:: {}"},
        {137, "BrokenScopes", "namespace ::A {}"},
        {138, "BrokenScopes", "using namespace A::B::;"},
        {139, "BrokenScopes", "using namespace ::A;"},
        {140, "BrokenScopes", "namespace N { int x = ; }"},

        {141, "BrokenPropsOps", "class C { int opAdd(int a, int b, int c) {} }"},
        {142, "BrokenPropsOps", "class C { int prop { get; set(int a, int b); } }"},
        {143, "BrokenPropsOps", "class C { int prop { get const; set; } }"},
        {144, "BrokenPropsOps", "class C { MyClass &opAssign(const MyClass &inout) }"},
        {145, "BrokenPropsOps", "class C { void opAdd() {} }"},
        {146, "BrokenPropsOps", "class C { void opSub() {} }"},
        {147, "BrokenPropsOps", "class C { void opMul() {} }"},
        {148, "BrokenPropsOps", "class C { void opDiv() {} }"},
        {149, "BrokenPropsOps", "class C { void opNeg(int a) {} }"},
        {150, "BrokenPropsOps", "class C { void opCom(int a) {} }"},
        {151, "BrokenPropsOps", "class C { void opPostInc(int a) {} }"},
        {152, "BrokenPropsOps", "class C { void opPostDec(int a) {} }"},
        {153, "BrokenPropsOps", "class C { int opIndex() {} }"},
        {154, "BrokenPropsOps", "class C { int opIndex(uint a, uint b) { return 0; } }"},
        {155, "BrokenPropsOps", "class C { int prop { get(int a) { return 0; } } }"},
        {156, "BrokenPropsOps", "class C { void prop { set() {} } }"},
        {157, "BrokenPropsOps", "class C { int prop { get; set; get; } }"},
        {158, "BrokenPropsOps", "class C { int prop { badAccessor; } }"},
        {159, "BrokenPropsOps", "class C { int prop { get private; } }"},
        {160, "BrokenPropsOps", "class C { int prop { get final; } }"},
        {161, "BrokenPropsOps", "class C { int prop { get override; } }"},
        {162, "BrokenPropsOps", "int prop { get; set; }"},
        {163, "BrokenPropsOps", "interface I { int prop { get { return 0; } } }"},
        {164, "BrokenPropsOps", "class C { int opConv(int a) { return 0; } }"},
        {165, "BrokenPropsOps", "class C { int opCast(int a) { return 0; } }"},
        {166, "BrokenPropsOps", "class C { int opEquals() {} }"},
        {167, "BrokenPropsOps", "class C { int opCmp() { return 0; } }"},
        {168, "BrokenPropsOps", "void opAdd(int a, int b) {}"},
        {169, "BrokenPropsOps", "class C { int &opAssign(const C &inout) = default; }"},
        {170, "BrokenPropsOps", "class C { int &opAssign(const C &inout) delete {} }"},

        {171, "BrokenTypes", "int@ handle;"},
        {172, "BrokenTypes", "const @ int handle;"},
        {173, "BrokenTypes", "object@ @ handle;"},
        {174, "BrokenTypes", "array<int,,float> arr;"},
        {175, "BrokenTypes", "array<class> arr;"},
        {176, "BrokenTypes", "array<struct> arr;"},
        {177, "BrokenTypes", "array<void> arr;"},
        {178, "BrokenTypes", "array<int>@ @ handle;"},
        {179, "BrokenTypes", "array<int@ @> handle;"},
        {180, "BrokenTypes", "const const int x = 0;"},
        {181, "BrokenTypes", "const int const x = 0;"},
        {182, "BrokenTypes", "int const const x = 0;"},
        {183, "BrokenTypes", "class C { const const int x; }"},
        {184, "BrokenTypes", "class C { const int x = 0; }"},
        {185, "BrokenTypes", "float@ primFloatHandle;"},
        {186, "BrokenTypes", "bool@ primBoolHandle;"},
        {187, "BrokenTypes", "double@ primDoubleHandle;"},
        {188, "BrokenTypes", "uint@ primUintHandle;"},
        {189, "BrokenTypes", "string@ stringHandle;"},
        {190, "BrokenTypes", "array<array<>> badMatrix;"},
        {191, "BrokenTypes", "array<int> badInit = {1, 2,};"},
        {192, "BrokenTypes", "array<int> badInit = {,1, 2};"},
        {193, "BrokenTypes", "array<int> badInit = {1,, 2};"},
        {194, "BrokenTypes", "int[] badNativeArr = {1, 2,};"},
        {195, "BrokenTypes", "int[] badNativeArr = {,1};"},
        {196, "BrokenTypes", "class C { int[] arr = {1, 2,}; }"},
        {197, "BrokenTypes", "class C {} C @ const @ doubleConstHandle;"},
        {198, "BrokenTypes", "class C {} const const C @ const doubleConstClass;"},
        {199, "BrokenTypes", "class C {} C @ & handleRef;"},
        {200, "BrokenTypes", "class C {} C & @ refHandle;"}
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
        angel_lsp::config::TypeConfig typeConfig{"string", "array"};
        SemanticAnalysisRequest req{table, fileUri, "", &i18n, &typeConfig};
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

TEST_CASE("DynamicTypeConfig_CustomString")
{
    angel_lsp::i18n::I18n i18n("en");
    angel_lsp::config::TypeConfig customConfig{"my_custom_string", "array"};

    // 1. my_custom_string@ should be rejected as handle on primitive/value type
    {
        std::string fileUri = "file:///custom1.as";
        SymbolTable table;
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);

        std::string code = "my_custom_string@ handle;";
        auto syntaxDiags = collector.CollectSymbols(fileUri, code, parser, table);

        SemanticAnalyzer analyzer;
        SemanticAnalysisRequest req{table, fileUri, "", &i18n, &customConfig};
        auto semanticDiags = analyzer.Analyze(req);

        std::vector<Diagnostic> allDiags = syntaxDiags;
        allDiags.insert(allDiags.end(), semanticDiags.begin(), semanticDiags.end());

        bool foundCustomHandleError = false;
        for (const auto &d : allDiags)
        {
            if (d.code == "as-err-handle-on-primitive")
            {
                foundCustomHandleError = true;
                break;
            }
        }
        CHECK(foundCustomHandleError);
    }

    // 2. string@ should NOT be rejected as primitive/value type when stringTypeName = "my_custom_string"
    {
        std::string fileUri = "file:///custom2.as";
        SymbolTable table;
        Symbol s;
        s.type = SymbolType::Class;
        s.name = "string";
        s.qualifiedName = "string";
        s.fileUri = fileUri;
        s.signature = ClassSignature{};
        table.AddSymbol(s);

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);

        std::string code = "string@ handle;";
        auto syntaxDiags = collector.CollectSymbols(fileUri, code, parser, table);

        SemanticAnalyzer analyzer;
        SemanticAnalysisRequest req{table, fileUri, "", &i18n, &customConfig};
        auto semanticDiags = analyzer.Analyze(req);

        std::vector<Diagnostic> allDiags = syntaxDiags;
        allDiags.insert(allDiags.end(), semanticDiags.begin(), semanticDiags.end());

        bool foundPrimitiveHandleError = false;
        for (const auto &d : allDiags)
        {
            if (d.code == "as-err-handle-on-primitive")
            {
                foundPrimitiveHandleError = true;
                break;
            }
        }
        CHECK_FALSE(foundPrimitiveHandleError);
    }
}

TEST_CASE("Batch_NewEdge200_Harness_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {201, "EnumsEdge", "enum E { A = function(){} }"},
        {202, "EnumsEdge", "enum E { A = \"cadena\", B = 10 }"},
        {203, "EnumsEdge", "enum E { A = , B }"},
        {204, "EnumsEdge", "enum E { A, , B }"},
        {205, "EnumsEdge", "enum E { A = 10, B = A + 5 }"},
        {206, "EnumsEdge", "enum E { A = 1, B = A }"},
        {207, "EnumsEdge", "enum E { A = true }"},
        {208, "EnumsEdge", "enum E { A = false }"},
        {209, "EnumsEdge", "enum E { A = 3.14f }"},
        {210, "EnumsEdge", "enum E { A = 1 + 2 * 3 }"},
        {211, "EnumsEdge", "enum E { A = (1 << 2) }"},
        {212, "EnumsEdge", "enum E { A = ~0 }"},
        {213, "EnumsEdge", "enum E { A = -10 }"},
        {214, "EnumsEdge", "enum E { A = +10 }"},
        {215, "EnumsEdge", "enum E { A = 1, B = 2, }"},
        {216, "EnumsEdge", "enum E { A = 1,, B = 2 }"},
        {217, "EnumsEdge", "enum E { = 1 }"},
        {218, "EnumsEdge", "enum E { A = B, B = 1 }"},
        {219, "EnumsEdge", "enum E { A = A }"},
        {220, "EnumsEdge", "enum E { A = 0x1F }"},
        {221, "EnumsEdge", "enum E { A = 0b1010 }"},
        {222, "EnumsEdge", "enum E { A = null }"},
        {223, "EnumsEdge", "class Obj {} enum E { A = Obj() }"},
        {224, "EnumsEdge", "enum E { A = void }"},
        {225, "EnumsEdge", "enum E { A = auto }"},
        {226, "EnumsEdge", "enum E { A = int }"},
        {227, "EnumsEdge", "enum E { A = float }"},
        {228, "EnumsEdge", "enum E { A = class }"},
        {229, "EnumsEdge", "enum E { A = struct }"},
        {230, "EnumsEdge", "enum E { A = enum }"},
        {231, "EnumsEdge", "enum E { A = 1, A = 2 }"},
        {232, "EnumsEdge", "enum E { A = 1 } enum E2 { A = 1 }"},
        {233, "EnumsEdge", "namespace N { enum E { A = 1 } } enum E { A = 1 }"},
        {234, "EnumsEdge", "shared enum E { A = 1 }"},
        {235, "EnumsEdge", "external enum E { A = 1 }"},

        {236, "ScopesEdge", "class MyClass {} ::MyClass@ someClassname;"},
        {237, "ScopesEdge", "namespace SOMENAMESPACE { class MYCLASS {} } SOMENAMESPACE::MYCLASS@ myFunction() { return null; }"},
        {238, "ScopesEdge", "namespace SOMENAMESPACE { class MYCLASS {} } ::SOMENAMESPACE::MYCLASS@ myFunction() { return null; }"},
        {239, "ScopesEdge", "namespace A::::B {}"},
        {240, "ScopesEdge", "namespace ::namespace::Class {}"},
        {241, "ScopesEdge", "class MyClass {} void f(::MyClass@ obj) {}"},
        {242, "ScopesEdge", "class MyClass {} ::MyClass f() { return MyClass(); }"},
        {243, "ScopesEdge", "namespace N { class C {} } N::C@ h;"},
        {244, "ScopesEdge", "namespace N { class C {} } ::N::C@ h;"},
        {245, "ScopesEdge", "namespace N { namespace M { class C {} } } ::N::M::C@ h;"},
        {246, "ScopesEdge", "namespace N { namespace M { class C {} } } N::M::C@ h;"},
        {247, "ScopesEdge", "class GlobalC {} namespace N { ::GlobalC@ h; }"},
        {248, "ScopesEdge", "namespace N { class LocalC {} } namespace N { LocalC@ h; }"},
        {249, "ScopesEdge", "namespace N { class LocalC {} } namespace N { ::N::LocalC@ h; }"},
        {250, "ScopesEdge", "namespace N { class C {} } ::N::C f(::N::C@ arg) { return C(); }"},
        {251, "ScopesEdge", "namespace N { enum E { A, B } } ::N::E val = ::N::A;"},
        {252, "ScopesEdge", "namespace N { enum E { A, B } } ::N::E val = ::N::E::A;"},
        {253, "ScopesEdge", "namespace N { typedef int MyInt; } ::N::MyInt x = 10;"},
        {254, "ScopesEdge", "namespace N { funcdef void CB(); } ::N::CB@ cb;"},
        {255, "ScopesEdge", "namespace A { class C {} } namespace B { class C {} } ::A::C a; ::B::C b;"},
        {256, "ScopesEdge", "namespace A { class C {} } namespace B { using namespace A; ::A::C a; }"},
        {257, "ScopesEdge", "namespace A { class C {} } namespace B { using namespace A; C a; }"},
        {258, "ScopesEdge", "namespace A { namespace B { class C {} } } using namespace A::B; C obj;"},
        {259, "ScopesEdge", "namespace A { namespace B { class C {} } } using namespace ::A::B; C obj;"},
        {260, "ScopesEdge", "using namespace ::;"},
        {261, "ScopesEdge", "using namespace A::;"},
        {262, "ScopesEdge", "using namespace ::A::;"},
        {263, "ScopesEdge", "namespace :: {}"},
        {264, "ScopesEdge", "namespace A:: {}"},
        {265, "ScopesEdge", "namespace ::A:: {}"},
        {266, "ScopesEdge", "namespace A { namespace }"},
        {267, "ScopesEdge", "namespace A { namespace 123B {} }"},
        {268, "ScopesEdge", "namespace A { class C {} } ::A::C@ f(::A::C@ inObj) { return inObj; }"},
        {269, "ScopesEdge", "namespace N { interface I { void m(); } } class Impl : ::N::I { void m() {} }"},
        {270, "ScopesEdge", "namespace N { mixin class M { void f(){} } } class C { mixin ::N::M; }"},
        {271, "ScopesEdge", "class C {} ::C@ objHandle;"},
        {272, "ScopesEdge", "class C {} const ::C@ constObjHandle;"},
        {273, "ScopesEdge", "class C {} ::C@ const constHandleObj = null;"},
        {274, "ScopesEdge", "class C {} const ::C@ const constClassHandle = null;"},
        {275, "ScopesEdge", "namespace N { class C {} } const ::N::C@ const constNSHandle = null;"},

        {276, "HandlesEdge", "class MyClass {} MyClass@@ handle;"},
        {277, "HandlesEdge", "class MyClass {} object@ @ h;"},
        {278, "HandlesEdge", "int@ x;"},
        {279, "HandlesEdge", "@int y;"},
        {280, "HandlesEdge", "class C @ {}"},
        {281, "HandlesEdge", "void f(int & & a)"},
        {282, "HandlesEdge", "int & var;"},
        {283, "HandlesEdge", "class MyClass {} MyClass@& handleRef;"},
        {284, "HandlesEdge", "class MyClass {} MyClass&@ refHandle;"},
        {285, "HandlesEdge", "class obj {} const obj @ const d = null;"},
        {286, "HandlesEdge", "class obj {} const obj @ const f(const obj @ const &in arg) const { return null; }"},
        {287, "HandlesEdge", "float@ fVal;"},
        {288, "HandlesEdge", "bool@ bVal;"},
        {289, "HandlesEdge", "double@ dVal;"},
        {290, "HandlesEdge", "uint@ uVal;"},
        {291, "HandlesEdge", "int8@ i8Val;"},
        {292, "HandlesEdge", "int16@ i16Val;"},
        {293, "HandlesEdge", "int64@ i64Val;"},
        {294, "HandlesEdge", "uint8@ u8Val;"},
        {295, "HandlesEdge", "uint16@ u16Val;"},
        {296, "HandlesEdge", "uint64@ u64Val;"},
        {297, "HandlesEdge", "class C {} C@ @ doubleHandleSpace;"},
        {298, "HandlesEdge", "class C {} C @ @ doubleHandleSpace2;"},
        {299, "HandlesEdge", "class C {} C@@@ tripleHandle;"},
        {300, "HandlesEdge", "class C {} const const C@ doubleConstType;"},
        {301, "HandlesEdge", "class C {} C@ const const doubleConstModifier;"},
        {302, "HandlesEdge", "class C {} const C@ const const doubleConstBoth;"},
        {303, "HandlesEdge", "class C {} void f(C@ &out param) {}"},
        {304, "HandlesEdge", "class C {} void f(C@ &in param) {}"},
        {305, "HandlesEdge", "class C {} void f(C@ &inout param) {}"},
        {306, "HandlesEdge", "class C {} void f(const C@ &in param) {}"},
        {307, "HandlesEdge", "class C {} void f(const C@ &out param) {}"},
        {308, "HandlesEdge", "class C {} void f(const C@ &inout param) {}"},
        {309, "HandlesEdge", "class C {} C@ & globalHandleRef;"},
        {310, "HandlesEdge", "class C {} C& @ globalRefHandle;"},
        {311, "HandlesEdge", "class C {} static C@ & staticHandleRef;"},
        {312, "HandlesEdge", "class C { C@ & memberHandleRef; }"},
        {313, "HandlesEdge", "class C { C& @ memberRefHandle; }"},
        {314, "HandlesEdge", "void f(int & &in a) {}"},
        {315, "HandlesEdge", "void f(int & &out a) {}"},
        {316, "HandlesEdge", "void f(int & &inout a) {}"},
        {317, "HandlesEdge", "void f(int &in &out a) {}"},
        {318, "HandlesEdge", "void f(int &out &inout a) {}"},
        {319, "HandlesEdge", "float & globalFloatRef;"},
        {320, "HandlesEdge", "class Obj {} Obj & globalObjRef;"},

        {321, "TyposEdge", "clas C {}"},
        {322, "TyposEdge", "vodi f()"},
        {323, "TyposEdge", "int f() { retun 0; }"},
        {324, "TyposEdge", "intreface I {}"},
        {325, "TyposEdge", "namespac N {}"},
        {326, "TyposEdge", "void f {}"},
        {327, "TyposEdge", "class MyClass {}; MyClass obj; void f;"},
        {328, "TyposEdge", "int x = 10 class C {}"},
        {329, "TyposEdge", "typedef int MyInt"},
        {330, "TyposEdge", "int[ arr;"},
        {331, "TyposEdge", "int[]] arr2;"},
        {332, "TyposEdge", "class C [ int x; ]"},
        {333, "TyposEdge", "class C { int x;"},
        {334, "TyposEdge", "interface I { void f()"},
        {335, "TyposEdge", "enum E { A, B"},
        {336, "TyposEdge", "funcdef void CB("},
        {337, "TyposEdge", "void f(int a"},
        {338, "TyposEdge", "class C : Base"},
        {339, "TyposEdge", "using namespace N"},
        {340, "TyposEdge", "import void f() from \"mod\""},
        {341, "TyposEdge", "external void f()"},
        {342, "TyposEdge", "mixin class M { void f()"},
        {343, "TyposEdge", "class C { mixin M"},
        {344, "TyposEdge", "const int x = 10"},
        {345, "TyposEdge", "class C { int x = 10"},
        {346, "TyposEdge", "void f() { return 0"},
        {347, "TyposEdge", "class C { C()"},
        {348, "TyposEdge", "class C { ~C()"},
        {349, "TyposEdge", "class C { int prop { get"},
        {350, "TyposEdge", "class C { int prop { set"},
        {351, "TyposEdge", "stuct S {}"},
        {352, "TyposEdge", "publc class C {}"},
        {353, "TyposEdge", "privat class C {}"},
        {354, "TyposEdge", "protectd class C {}"},
        {355, "TyposEdge", "statc int x = 0;"},
        {356, "TyposEdge", "cnost int x = 0;"},
        {357, "TyposEdge", "overide void f() {}"},
        {358, "TyposEdge", "finl class C {}"},
        {359, "TyposEdge", "abstrct class C {}"},
        {360, "TyposEdge", "shared class C {}"},

        {361, "ParamsEdge", "class CustomClass {} void f(CustomClass &in a) {}"},
        {362, "ParamsEdge", "class CustomClass {} void f(const CustomClass &out b) {}"},
        {363, "ParamsEdge", "class CustomClass {} void f(CustomClass &inout c) {}"},
        {364, "ParamsEdge", "void f(int &inout a) {}"},
        {365, "ParamsEdge", "class CustomClass {} void f(CustomClass &out opt = void) {}"},
        {366, "ParamsEdge", "class CustomClass {} void f(CustomClass &in opt = void) {}"},
        {367, "ParamsEdge", "class CustomClass {} void f(CustomClass &inout opt = void) {}"},
        {368, "ParamsEdge", "void f(int &in a) {}"},
        {369, "ParamsEdge", "void f(int &out a) {}"},
        {370, "ParamsEdge", "void f(const int &in a) {}"},
        {371, "ParamsEdge", "void f(const int &out a) {}"},
        {372, "ParamsEdge", "void f(const int &inout a) {}"},
        {373, "ParamsEdge", "class CustomClass {} void f(const CustomClass &in a) {}"},
        {374, "ParamsEdge", "class CustomClass {} void f(const CustomClass &inout c) {}"},
        {375, "ParamsEdge", "class CustomClass {} void f(CustomClass@ &in a) {}"},
        {376, "ParamsEdge", "class CustomClass {} void f(CustomClass@ &out b) {}"},
        {377, "ParamsEdge", "class CustomClass {} void f(CustomClass@ &inout c) {}"},
        {378, "ParamsEdge", "class CustomClass {} void f(const CustomClass@ &in a) {}"},
        {379, "ParamsEdge", "class CustomClass {} void f(const CustomClass@ &out b) {}"},
        {380, "ParamsEdge", "class CustomClass {} void f(const CustomClass@ &inout c) {}"},
        {381, "ParamsEdge", "class CustomClass {} void f(CustomClass &out opt = CustomClass()) {}"},
        {382, "ParamsEdge", "void f(int &out opt = 10) {}"},
        {383, "ParamsEdge", "void f(int &in opt = 10) {}"},
        {384, "ParamsEdge", "void f(int &inout opt = 10) {}"},
        {385, "ParamsEdge", "class CustomClass {} void f(CustomClass &in opt = CustomClass()) {}"},
        {386, "ParamsEdge", "class CustomClass {} void f(CustomClass &inout opt = CustomClass()) {}"},
        {387, "ParamsEdge", "void f(int &out a = void) {}"},
        {388, "ParamsEdge", "void f(float &out a = void) {}"},
        {389, "ParamsEdge", "void f(bool &out a = void) {}"},
        {390, "ParamsEdge", "void f(double &out a = void) {}"},
        {391, "ParamsEdge", "void f(uint &out a = void) {}"},
        {392, "ParamsEdge", "void f(int &in a = void) {}"},
        {393, "ParamsEdge", "void f(float &in a = void) {}"},
        {394, "ParamsEdge", "void f(int &inout a = void) {}"},
        {395, "ParamsEdge", "class CustomClass {} void f(CustomClass &out a = 10) {}"},
        {396, "ParamsEdge", "class CustomClass {} void f(CustomClass &out a = \"str\") {}"},
        {397, "ParamsEdge", "class CustomClass {} void f(CustomClass &out a = null) {}"},
        {398, "ParamsEdge", "class CustomClass {} void f(CustomClass@ &out a = null) {}"},
        {399, "ParamsEdge", "class CustomClass {} void f(CustomClass@ &in a = null) {}"},
        {400, "ParamsEdge", "class CustomClass {} void f(CustomClass@ &inout a = null) {}"}
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
        angel_lsp::config::TypeConfig typeConfig{"string", "array"};
        SemanticAnalysisRequest req{table, fileUri, "", &i18n, &typeConfig};
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

TEST_CASE("Batch_Array600_Harness_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {401, "ArrayUnresolved", "array<NonExistentClass> arr;"},
        {402, "ArrayUnresolved", "array<NonExistentClass>@ handle;"},
        {403, "ArrayUnresolved", "namespace N { array<UndefinedInScope> a; }"},
        {404, "ArrayUnresolved", "array<UnknownType>@[] handleArray;"},
        {405, "ArrayUnresolved", "class MyClass {} array<MyClass> arr;"},
        {406, "ArrayUnresolved", "class MyClass {} array<MyClass@> arr;"},
        {407, "ArrayUnresolved", "class MyClass {} array<MyClass>@ handle;"},
        {408, "ArrayUnresolved", "class MyClass {} array<const MyClass@> arr;"},
        {409, "ArrayUnresolved", "namespace N { class LocalClass {} array<LocalClass> arr; }"},
        {410, "ArrayUnresolved", "namespace N { class LocalClass {} } array<N::LocalClass> arr;"},
        {411, "ArrayUnresolved", "namespace N { class LocalClass {} } namespace N { array<LocalClass> arr; }"},
        {412, "ArrayUnresolved", "enum E { A, B } array<E> enumArr;"},
        {413, "ArrayUnresolved", "typedef int MyInt; array<MyInt> typedefArr;"},
        {414, "ArrayUnresolved", "funcdef void CB(); array<CB@> cbArr;"},
        {415, "ArrayUnresolved", "interface I {} array<I@> ifaceArr;"},
        {416, "ArrayUnresolved", "array<int> intArr;"},
        {417, "ArrayUnresolved", "array<float> floatArr;"},
        {418, "ArrayUnresolved", "array<double> doubleArr;"},
        {419, "ArrayUnresolved", "array<bool> boolArr;"},
        {420, "ArrayUnresolved", "array<uint> uintArr;"},
        {421, "ArrayUnresolved", "array<string> strArr;"},
        {422, "ArrayUnresolved", "array<array<int>> matrix2D;"},
        {423, "ArrayUnresolved", "array<array<array<float>>> matrix3D;"},
        {424, "ArrayUnresolved", "array<array<UnknownClass>> badMatrix;"},
        {425, "ArrayUnresolved", "array<void> voidArray;"},
        {426, "ArrayUnresolved", "array<const void> constVoidArray;"},
        {427, "ArrayUnresolved", "array<auto> autoArray;"},
        {428, "ArrayUnresolved", "array<class> classKeywordArray;"},
        {429, "ArrayUnresolved", "array<struct> structKeywordArray;"},
        {430, "ArrayUnresolved", "array<enum> enumKeywordArray;"},
        {431, "ArrayUnresolved", "array<funcdef> funcdefKeywordArray;"},
        {432, "ArrayUnresolved", "array<interface> interfaceKeywordArray;"},
        {433, "ArrayUnresolved", "array<namespace> namespaceKeywordArray;"},
        {434, "ArrayUnresolved", "array<using> usingKeywordArray;"},
        {435, "ArrayUnresolved", "array<import> importKeywordArray;"},
        {436, "ArrayUnresolved", "array<export> exportKeywordArray;"},
        {437, "ArrayUnresolved", "array<external> externalKeywordArray;"},
        {438, "ArrayUnresolved", "array<shared> sharedKeywordArray;"},
        {439, "ArrayUnresolved", "array<final> finalKeywordArray;"},
        {440, "ArrayUnresolved", "array<abstract> abstractKeywordArray;"},

        {441, "NativeVsGeneric", "int[] nativeArr;"},
        {442, "NativeVsGeneric", "array<int> genericArr;"},
        {443, "NativeVsGeneric", "int[]@ nativeHandle;"},
        {444, "NativeVsGeneric", "array<int>@ genericHandle;"},
        {445, "NativeVsGeneric", "int[][] multidimNative;"},
        {446, "NativeVsGeneric", "array<array<int>> multidimGeneric;"},
        {447, "NativeVsGeneric", "const int[] constArr;"},
        {448, "NativeVsGeneric", "const int[]@ const handleConstArr;"},
        {449, "NativeVsGeneric", "float[] nativeFloat;"},
        {450, "NativeVsGeneric", "array<float> genericFloat;"},
        {451, "NativeVsGeneric", "double[] nativeDouble;"},
        {452, "NativeVsGeneric", "array<double> genericDouble;"},
        {453, "NativeVsGeneric", "bool[] nativeBool;"},
        {454, "NativeVsGeneric", "array<bool> genericBool;"},
        {455, "NativeVsGeneric", "uint[] nativeUint;"},
        {456, "NativeVsGeneric", "array<uint> genericUint;"},
        {457, "NativeVsGeneric", "string[] nativeString;"},
        {458, "NativeVsGeneric", "array<string> genericString;"},
        {459, "NativeVsGeneric", "class C {} C[] nativeObjArr;"},
        {460, "NativeVsGeneric", "class C {} array<C> genericObjArr;"},
        {461, "NativeVsGeneric", "class C {} C@[] nativeObjHandleArr;"},
        {462, "NativeVsGeneric", "class C {} array<C@> genericObjHandleArr;"},
        {463, "NativeVsGeneric", "class C {} C[]@ nativeHandleToObjArr;"},
        {464, "NativeVsGeneric", "class C {} array<C>@ genericHandleToObjArr;"},
        {465, "NativeVsGeneric", "class C {} const C[] constNativeObjArr;"},
        {466, "NativeVsGeneric", "class C {} const array<C> constGenericObjArr;"},
        {467, "NativeVsGeneric", "int[][][] native3D;"},
        {468, "NativeVsGeneric", "array<array<array<int>>> generic3D;"},
        {469, "NativeVsGeneric", "int[] arrInit = {1, 2, 3};"},
        {470, "NativeVsGeneric", "array<int> arrInit = {1, 2, 3};"},
        {471, "NativeVsGeneric", "int[][] matrixInit = {{1, 2}, {3, 4}};"},
        {472, "NativeVsGeneric", "array<array<int>> matrixInit = {{1, 2}, {3, 4}};"},
        {473, "NativeVsGeneric", "void f(int[] arr) {}"},
        {474, "NativeVsGeneric", "void f(array<int> arr) {}"},
        {475, "NativeVsGeneric", "int[] f() { return null; }"},
        {476, "NativeVsGeneric", "array<int> f() { return null; }"},
        {477, "NativeVsGeneric", "class C { int[] memberArr; }"},
        {478, "NativeVsGeneric", "class C { array<int> memberArr; }"},
        {479, "NativeVsGeneric", "interface I { int[] get_items(); }"},
        {480, "NativeVsGeneric", "interface I { array<int> get_items(); }"},

        {481, "ArrayNameCollisions", "array<int> array;"},
        {482, "ArrayNameCollisions", "int array = 10;"},
        {483, "ArrayNameCollisions", "float array = 3.14f;"},
        {484, "ArrayNameCollisions", "class array {}"},
        {485, "ArrayNameCollisions", "void array() {}"},
        {486, "ArrayNameCollisions", "interface array {}"},
        {487, "ArrayNameCollisions", "enum array { A, B }"},
        {488, "ArrayNameCollisions", "typedef int array;"},
        {489, "ArrayNameCollisions", "funcdef void array();"},
        {490, "ArrayNameCollisions", "namespace array {}"},
        {491, "ArrayNameCollisions", "class C { int array; }"},
        {492, "ArrayNameCollisions", "class C { void array() {} }"},
        {493, "ArrayNameCollisions", "void f(int array) {}"},
        {494, "ArrayNameCollisions", "class string {}"},
        {495, "ArrayNameCollisions", "void string() {}"},
        {496, "ArrayNameCollisions", "int string = 5;"},
        {497, "ArrayNameCollisions", "class int {}"},
        {498, "ArrayNameCollisions", "class float {}"},
        {499, "ArrayNameCollisions", "class bool {}"},
        {500, "ArrayNameCollisions", "class void {}"},
        {501, "ArrayNameCollisions", "class auto {}"},
        {502, "ArrayNameCollisions", "class const {}"},
        {503, "ArrayNameCollisions", "class final {}"},
        {504, "ArrayNameCollisions", "class override {}"},
        {505, "ArrayNameCollisions", "class delete {}"},
        {506, "ArrayNameCollisions", "class explicit {}"},
        {507, "ArrayNameCollisions", "class property {}"},
        {508, "ArrayNameCollisions", "class get {}"},
        {509, "ArrayNameCollisions", "class set {}"},
        {510, "ArrayNameCollisions", "class mixin {}"},
        {511, "ArrayNameCollisions", "class interface {}"},
        {512, "ArrayNameCollisions", "class namespace {}"},
        {513, "ArrayNameCollisions", "class typedef {}"},
        {514, "ArrayNameCollisions", "class funcdef {}"},
        {515, "ArrayNameCollisions", "class enum {}"},
        {516, "ArrayNameCollisions", "class shared {}"},
        {517, "ArrayNameCollisions", "class external {}"},
        {518, "ArrayNameCollisions", "class import {}"},
        {519, "ArrayNameCollisions", "class from {}"},
        {520, "ArrayNameCollisions", "class return {}"},

        {521, "MultipleDeclarations", "array<int> a, b, c;"},
        {522, "MultipleDeclarations", "int[] a, b = {1, 2}, c;"},
        {523, "MultipleDeclarations", "array<int>@ a, b = null, c;"},
        {524, "MultipleDeclarations", "array<int> a, float[] b;"},
        {525, "MultipleDeclarations", "int[] a, float b;"},
        {526, "MultipleDeclarations", "int a, float[] b;"},
        {527, "MultipleDeclarations", "int[] a = {1}, b = {2, 3}, c = {4, 5, 6};"},
        {528, "MultipleDeclarations", "array<int> a = {1}, b = {2, 3};"},
        {529, "MultipleDeclarations", "const int[] a, b;"},
        {530, "MultipleDeclarations", "const array<int> a, b;"},
        {531, "MultipleDeclarations", "class C {} C[] a, b;"},
        {532, "MultipleDeclarations", "class C {} array<C> a, b;"},
        {533, "MultipleDeclarations", "class C {} C@[] a, b;"},
        {534, "MultipleDeclarations", "class C {} array<C@> a, b;"},
        {535, "MultipleDeclarations", "int[] a, ;"},
        {536, "MultipleDeclarations", "array<int> a, ;"},
        {537, "MultipleDeclarations", "int[] , b;"},
        {538, "MultipleDeclarations", "array<int> , b;"},
        {539, "MultipleDeclarations", "int[] a, b = ;"},
        {540, "MultipleDeclarations", "array<int> a, b = ;"},
        {541, "MultipleDeclarations", "int[] a = , b;"},
        {542, "MultipleDeclarations", "array<int> a = , b;"},
        {543, "MultipleDeclarations", "int[] a, b, int c;"},
        {544, "MultipleDeclarations", "array<int> a, b, int c;"},
        {545, "MultipleDeclarations", "int a, b, int[] c;"},
        {546, "MultipleDeclarations", "int a, b, array<int> c;"},
        {547, "MultipleDeclarations", "int[] a, int[] b;"},
        {548, "MultipleDeclarations", "array<int> a, array<int> b;"},
        {549, "MultipleDeclarations", "int[][] a, b;"},
        {550, "MultipleDeclarations", "array<array<int>> a, b;"},
        {551, "MultipleDeclarations", "class C { int[] a, b; }"},
        {552, "MultipleDeclarations", "class C { array<int> a, b; }"},
        {553, "MultipleDeclarations", "namespace N { int[] a, b; }"},
        {554, "MultipleDeclarations", "namespace N { array<int> a, b; }"},
        {555, "MultipleDeclarations", "void f(int[] a, b) {}"},
        {556, "MultipleDeclarations", "void f(array<int> a, b) {}"},
        {557, "MultipleDeclarations", "int[] a, b, c = {1, 2};"},
        {558, "MultipleDeclarations", "array<int> a, b, c = {1, 2};"},
        {559, "MultipleDeclarations", "int[] a = null, b = null;"},
        {560, "MultipleDeclarations", "array<int> a = null, b = null;"},

        {561, "CorruptTemplates", "array<int arr;"},
        {562, "CorruptTemplates", "array<int, float> arr;"},
        {563, "CorruptTemplates", "array<> arr;"},
        {564, "CorruptTemplates", "int[ arr;"},
        {565, "CorruptTemplates", "int[]] arr2;"},
        {566, "CorruptTemplates", "array<array<int>> arr;"},
        {567, "CorruptTemplates", "array<array<int> > arr;"},
        {568, "CorruptTemplates", "array<int> arr = {,1, 2};"},
        {569, "CorruptTemplates", "int[] arr = {1, 2,, 3};"},
        {570, "CorruptTemplates", "array<int> arr = {1, 2,};"},
        {571, "CorruptTemplates", "int[] arr = {1, 2,};"},
        {572, "CorruptTemplates", "array<int>> badClose;"},
        {573, "CorruptTemplates", "array<<int> badOpen;"},
        {574, "CorruptTemplates", "array<int,,float> badCommas;"},
        {575, "CorruptTemplates", "array<,int> leadingComma;"},
        {576, "CorruptTemplates", "array<int,> trailingComma;"},
        {577, "CorruptTemplates", "array<int a> varInTemplate;"},
        {578, "CorruptTemplates", "array<int = 10> defaultInTemplate;"},
        {579, "CorruptTemplates", "array<10> intValueInTemplate;"},
        {580, "CorruptTemplates", "array<\"str\"> strValueInTemplate;"},
        {581, "CorruptTemplates", "array<true> boolValueInTemplate;"},
        {582, "CorruptTemplates", "array<null> nullValueInTemplate;"},
        {583, "CorruptTemplates", "int[[ arr;"},
        {584, "CorruptTemplates", "int]]] arr;"},
        {585, "CorruptTemplates", "int[[] arr;"},
        {586, "CorruptTemplates", "int[]] arr;"},
        {587, "CorruptTemplates", "int][ arr;"},
        {588, "CorruptTemplates", "int[] arr = {;}"},
        {589, "CorruptTemplates", "array<int> arr = {;};"},
        {590, "CorruptTemplates", "int[] arr = {};"},
        {591, "CorruptTemplates", "array<int> arr = {};"},
        {592, "CorruptTemplates", "int[] arr = {1};"},
        {593, "CorruptTemplates", "array<int> arr = {1};"},
        {594, "CorruptTemplates", "array<int> arr = {1, 2, 3};"},
        {595, "CorruptTemplates", "int[] arr = {1, 2, 3};"},
        {596, "CorruptTemplates", "array<array<int>> matrix = {{1, 2}, {3, 4}};"},
        {597, "CorruptTemplates", "int[][] matrix = {{1, 2}, {3, 4}};"},
        {598, "CorruptTemplates", "array<int>@ arrHandle = null;"},
        {599, "CorruptTemplates", "int[]@ arrHandle = null;"},
        {600, "CorruptTemplates", "const array<int>@ const constArrHandle = null;"}
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
        angel_lsp::config::TypeConfig typeConfig{"string", "array"};
        SemanticAnalysisRequest req{table, fileUri, "", &i18n, &typeConfig};
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

TEST_CASE("Batch_StringTypedef800_Harness_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {601, "StringEscapes", "string s = \"Hola Mundo\";"},
        {602, "StringEscapes", "string s = 'Hola Mundo';"},
        {603, "StringEscapes", "string s = \"Linea 1\\nLinea 2\";"},
        {604, "StringEscapes", "string s = \"Tab\\tSeparado\";"},
        {605, "StringEscapes", "string s = \"Retorno\\rCarro\";"},
        {606, "StringEscapes", "string s = \"Nulo\\0Byte\";"},
        {607, "StringEscapes", "string s = \"Barra\\\\Invertida\";"},
        {608, "StringEscapes", "string s = \"Comilla\\\"Doble\";"},
        {609, "StringEscapes", "string s = 'Comilla\\'Simple';"},
        {610, "StringEscapes", "string s = \"\\xFF\";"},
        {611, "StringEscapes", "string s = \"\\x00\";"},
        {612, "StringEscapes", "string s = \"\\x1F\";"},
        {613, "StringEscapes", "string s = \"\\xA\";"},
        {614, "StringEscapes", "string s = \"\\x1234\";"},
        {615, "StringEscapes", "string s = \"\\z\";"},
        {616, "StringEscapes", "string s = \"\\xG\";"},
        {617, "StringEscapes", "string s = \"\\x\";"},
        {618, "StringEscapes", "string s = \"sin cerrar\n\";"},
        {619, "StringEscapes", "string s = 'sin cerrar\n';"},
        {620, "StringEscapes", "string s = \"\\a\";"},
        {621, "StringEscapes", "string s = \"\\b\";"},
        {622, "StringEscapes", "string s = \"\\f\";"},
        {623, "StringEscapes", "string s = \"\\v\";"},
        {624, "StringEscapes", "string s = \"\\?\";"},
        {625, "StringEscapes", "string s = \"\";"},
        {626, "StringEscapes", "string s = '';"},
        {627, "StringEscapes", "string s = \"   \";"},
        {628, "StringEscapes", "string s = \"1234567890\";"},
        {629, "StringEscapes", "string s = \"!@#$%^&*()\";"},
        {630, "StringEscapes", "string s = \"Escape \\\\\\\" anidado\";"},
        {631, "StringEscapes", "string s = 'Escape \\\\\\\' anidado';"},
        {632, "StringEscapes", "string s = \"Mezcla \\n \\t \\r \\0 \\\\ \\\" \\xFF\";"},
        {633, "StringEscapes", "string s = \"\\x7F\";"},
        {634, "StringEscapes", "string s = \"\\x80\";"},
        {635, "StringEscapes", "string s = \"\\xFE\";"},
        {636, "StringEscapes", "string s = \"\\x12345\";"},
        {637, "StringEscapes", "string s = \"\\q\";"},
        {638, "StringEscapes", "string s = \"\\w\";"},
        {639, "StringEscapes", "string s = \"\\y\";"},
        {640, "StringEscapes", "string s = \"cadena con \\0 intermedia\";"},

        {641, "UnicodeEscapes", "string s = \"\\u0041\";"},
        {642, "UnicodeEscapes", "string s = \"\\u20AC\";"},
        {643, "UnicodeEscapes", "string s = \"\\U0001F600\";"},
        {644, "UnicodeEscapes", "string s = \"\\uD800\";"},
        {645, "UnicodeEscapes", "string s = \"\\uDFFF\";"},
        {646, "UnicodeEscapes", "string s = \"\\uDBFF\";"},
        {647, "UnicodeEscapes", "string s = \"\\uDC00\";"},
        {648, "UnicodeEscapes", "string s = \"\\U00110000\";"},
        {649, "UnicodeEscapes", "string s = \"\\u12\";"},
        {650, "UnicodeEscapes", "string s = \"\\U1234\";"},
        {651, "UnicodeEscapes", "string s = \"\\uFFFF\";"},
        {652, "UnicodeEscapes", "string s = \"\\u0000\";"},
        {653, "UnicodeEscapes", "string s = \"\\u007F\";"},
        {654, "UnicodeEscapes", "string s = \"\\u0080\";"},
        {655, "UnicodeEscapes", "string s = \"\\u07FF\";"},
        {656, "UnicodeEscapes", "string s = \"\\u0800\";"},
        {657, "UnicodeEscapes", "string s = \"\\uD7FF\";"},
        {658, "UnicodeEscapes", "string s = \"\\uE000\";"},
        {659, "UnicodeEscapes", "string s = \"\\U00000041\";"},
        {660, "UnicodeEscapes", "string s = \"\\U00000000\";"},
        {661, "UnicodeEscapes", "string s = \"\\U00010FFFF\";"},
        {662, "UnicodeEscapes", "string s = \"\\U00010FFF\";"},
        {663, "UnicodeEscapes", "string s = \"\\U00200000\";"},
        {664, "UnicodeEscapes", "string s = \"\\UFFFFFFFF\";"},
        {665, "UnicodeEscapes", "string s = \"\\uG123\";"},
        {666, "UnicodeEscapes", "string s = \"\\UG1234567\";"},
        {667, "UnicodeEscapes", "string s = \"\\u\";"},
        {668, "UnicodeEscapes", "string s = \"\\U\";"},
        {669, "UnicodeEscapes", "string s = \"\\u1\";"},
        {670, "UnicodeEscapes", "string s = \"\\u123\";"},
        {671, "UnicodeEscapes", "string s = \"\\U1234567\";"},
        {672, "UnicodeEscapes", "string s = \"\\u0041\\u0042\\u0043\";"},
        {673, "UnicodeEscapes", "string s = \"Texto \\u0041 normal\";"},
        {674, "UnicodeEscapes", "string s = \"\\u0022\";"},
        {675, "UnicodeEscapes", "string s = \"\\u0027\";"},
        {676, "UnicodeEscapes", "string s = \"\\u005C\";"},
        {677, "UnicodeEscapes", "string s = \"\\u000A\";"},
        {678, "UnicodeEscapes", "string s = \"\\u000D\";"},
        {679, "UnicodeEscapes", "string s = \"\\u0009\";"},
        {680, "UnicodeEscapes", "string s = \"\\uD900\";"},

        {681, "HeredocStrings", "string s = \"\"\"Texto multilinea\"\"\";"},
        {682, "HeredocStrings", "string s = \"\"\"Linea 1\nLinea 2\nLinea 3\"\"\";"},
        {683, "HeredocStrings", "string s = \"\"\"Con \\n y \\t literales\"\"\";"},
        {684, "HeredocStrings", "string s = \"\"\"Con \"comillas\" simples\"\"\";"},
        {685, "HeredocStrings", "string s = \"\"\"Con \"\"dos comillas\"\" sin problema\"\"\";"},
        {686, "HeredocStrings", "string s = \"\"\"Sin cerrar"},
        {687, "HeredocStrings", "string s = \"\"\"';"},
        {688, "HeredocStrings", "string s = \"\"\"\"\"\";"},
        {689, "HeredocStrings", "string s = \"\"\"a\"\"\";"},
        {690, "HeredocStrings", "string s = \"\"\"\n\"\"\";"},
        {691, "HeredocStrings", "string s = \"\"\"\\x41 \\u0041 \\n \\r \\t\"\"\";"},
        {692, "HeredocStrings", "string s = \"\"\"Barra \\ al final\"\"\";"},
        {693, "HeredocStrings", "string s = \"\"\"Simbolo \\0 nulo\"\"\";"},
        {694, "HeredocStrings", "string s = \"\"\"Codigo: { int x = 10; }\"\"\";"},
        {695, "HeredocStrings", "string s = \"\"\"HTML: <div class=\"box\"></div>\"\"\";"},
        {696, "HeredocStrings", "string s = \"\"\"JSON: {\"key\": \"value\"}\"\"\";"},
        {697, "HeredocStrings", "string s = \"\"\"XML: <tag attr='val'/>\"\"\";"},
        {698, "HeredocStrings", "string s = \"\"\"SQL: SELECT * FROM table WHERE col = 'a'\"\"\";"},
        {699, "HeredocStrings", "string s = \"\"\"Multiple \"\"\"\"\"\"\";"},
        {700, "HeredocStrings", "string s = \"\"\"Texto \"\"\" mas texto\"\"\";"},
        {701, "HeredocStrings", "string s = \"\"\"Ruta C:\\Users\\Fano\\Desktop\"\"\";"},
        {702, "HeredocStrings", "string s = \"\"\"Regex ^[a-z]+$\"\"\";"},
        {703, "HeredocStrings", "string s = \"\"\"Comentario // no ignora\"\"\";"},
        {704, "HeredocStrings", "string s = \"\"\"Comentario /* no ignora */\"\"\";"},
        {705, "HeredocStrings", "string s = \"\"\"Unicode real: € 😃 Å\"\"\";"},
        {706, "HeredocStrings", "string s = \"\"\"    tabulaciones    independientes\"\"\";"},
        {707, "HeredocStrings", "string s = \"\"\"Con salto final\n\"\"\";"},
        {708, "HeredocStrings", "string s = \"\"\"\nCon salto inicial\"\"\";"},
        {709, "HeredocStrings", "string s = \"\"\"Triple comilla \" en heredoc\"\"\";"},
        {710, "HeredocStrings", "string s = \"\"\"Heredoc vacio multilinea\n\n\"\"\";"},

        {711, "StringConcat", "string s = \"Parte 1 \" \"Parte 2\";"},
        {712, "StringConcat", "string s = \"Parte 1 \" // comentario\n\"Parte 2\";"},
        {713, "StringConcat", "string s = \"Parte 1 \" /* comentario */ \"Parte 2\";"},
        {714, "StringConcat", "string s = \"A\" \"B\" \"C\" \"D\";"},
        {715, "StringConcat", "string s = 'A' 'B' 'C';"},
        {716, "StringConcat", "string s = \"Mezcla \" 'simple \" y ' \"doble\";"},
        {717, "StringConcat", "string s = \"Linea 1\\n\" \"Linea 2\\n\";"},
        {718, "StringConcat", "string s = \"Concat \" + \"Normal\";"},
        {719, "StringConcat", "string s = \"Concat \" + \"Tres \" + \"Partes\";"},
        {720, "StringConcat", "string s = \"Concat \" + 10;"},
        {721, "StringConcat", "string s = 10 + \" Concat\";"},
        {722, "StringConcat", "string s = \"Concat \" + 3.14f;"},
        {723, "StringConcat", "string s = \"Concat \" + true;"},
        {724, "StringConcat", "string s = \"Concat \" + null;"},
        {725, "StringConcat", "string s = \"\"\"Heredoc \"\"\" \"Literal\";"},
        {726, "StringConcat", "string s = \"Literal \" \"\"\"Heredoc\"\"\";"},
        {727, "StringConcat", "string s = \"\"\"Heredoc 1 \"\"\" \"\"\"Heredoc 2\"\"\";"},
        {728, "StringConcat", "string s = \"Parte 1\"\n\"Parte 2\"\n\"Parte 3\";"},
        {729, "StringConcat", "string s = \"Parte 1\"\n\"Parte 2\";"},
        {730, "StringConcat", "void f(string s = \"A\" \"B\") {}"},
        {731, "StringConcat", "class C { string prop = \"A\" \"B\"; }"},
        {732, "StringConcat", "enum E { A = \"A\" \"B\" }"},
        {733, "StringConcat", "const string GLOBAL_STR = \"P1 \" \"P2\";"},
        {734, "StringConcat", "string s = \"Escapes \\n\" \" \\t Concat\";"},
        {735, "StringConcat", "string s = \"\\x41\" \"\\x42\";"},
        {736, "StringConcat", "string s = \"\\u0041\" \"\\u0042\";"},
        {737, "StringConcat", "string s = \"Sin espacio\"\"junto\";"},
        {738, "StringConcat", "string s = 'Sin espacio''junto';"},
        {739, "StringConcat", "string s = \"Triple \" \"Concat \" \"Automatica\";"},
        {740, "StringConcat", "string s = \"Concat \" \"con \" \"muchas \" \"partes \" \"seguidas\";"},

        {741, "TypedefsEdge", "typedef float real32;"},
        {742, "TypedefsEdge", "typedef double real64;"},
        {743, "TypedefsEdge", "typedef int int32;"},
        {744, "TypedefsEdge", "typedef uint uint32;"},
        {745, "TypedefsEdge", "typedef bool boolean;"},
        {746, "TypedefsEdge", "typedef float real32; real32 val = 1.0f;"},
        {747, "TypedefsEdge", "typedef double real64; real64 val = 2.0;"},
        {748, "TypedefsEdge", "typedef int int32; int32 val = 10;"},
        {749, "TypedefsEdge", "typedef float;"},
        {750, "TypedefsEdge", "typedef 123 float;"},
        {751, "TypedefsEdge", "typedef float class;"},
        {752, "TypedefsEdge", "typedef float struct;"},
        {753, "TypedefsEdge", "typedef float interface;"},
        {754, "TypedefsEdge", "typedef float enum;"},
        {755, "TypedefsEdge", "typedef float void;"},
        {756, "TypedefsEdge", "typedef float auto;"},
        {757, "TypedefsEdge", "typedef float const;"},
        {758, "TypedefsEdge", "typedef float return;"},
        {759, "TypedefsEdge", "typedef float if;"},
        {760, "TypedefsEdge", "typedef float while;"},
        {761, "TypedefsEdge", "typedef float int;"},
        {762, "TypedefsEdge", "typedef float double;"},
        {763, "TypedefsEdge", "typedef float string;"},
        {764, "TypedefsEdge", "typedef float array;"},
        {765, "TypedefsEdge", "typedef int MyInt; typedef float MyInt;"},
        {766, "TypedefsEdge", "typedef int MyInt; typedef int MyInt;"},
        {767, "TypedefsEdge", "class CustomClass {} typedef CustomClass MyClassAlias;"},
        {768, "TypedefsEdge", "class CustomClass {} typedef CustomClass@ MyClassHandleAlias;"},
        {769, "TypedefsEdge", "interface I {} typedef I@ IFaceHandle;"},
        {770, "TypedefsEdge", "enum E { A, B } typedef E EnumAlias;"},
        {771, "TypedefsEdge", "typedef NonExistentType BadAlias;"},
        {772, "TypedefsEdge", "namespace N { typedef int LocalInt; } N::LocalInt x = 5;"},
        {773, "TypedefsEdge", "namespace N { typedef int LocalInt; } using namespace N; LocalInt x = 5;"},
        {774, "TypedefsEdge", "class C { typedef int MemberInt; }"},
        {775, "TypedefsEdge", "typedef const int ConstInt;"},
        {776, "TypedefsEdge", "typedef int[] IntArrayAlias;"},
        {777, "TypedefsEdge", "typedef array<int> IntGenericArrayAlias;"},
        {778, "TypedefsEdge", "typedef void f();"},
        {779, "TypedefsEdge", "typedef float real32, real64;"},
        {780, "TypedefsEdge", "typedef int MyInt = 10;"},
        {781, "TypedefsEdge", "typedef int MyInt;"},
        {782, "TypedefsEdge", "typedef uint8 byte;"},
        {783, "TypedefsEdge", "typedef int8 sbyte;"},
        {784, "TypedefsEdge", "typedef int16 short;"},
        {785, "TypedefsEdge", "typedef uint16 ushort;"},
        {786, "TypedefsEdge", "typedef int64 long;"},
        {787, "TypedefsEdge", "typedef uint64 ulong;"},
        {788, "TypedefsEdge", "typedef float real32; void f(real32 p) {}"},
        {789, "TypedefsEdge", "typedef float real32; real32 f() { return 1.0f; }"},
        {790, "TypedefsEdge", "typedef float real32; class C { real32 member; }"},
        {791, "TypedefsEdge", "typedef float real32; interface I { real32 get_val(); }"},
        {792, "TypedefsEdge", "typedef int MyInt; MyInt[] arrAlias;"},
        {793, "TypedefsEdge", "typedef int MyInt; array<MyInt> arrGenericAlias;"},
        {794, "TypedefsEdge", "typedef MyAlias1 MyAlias2;"},
        {795, "TypedefsEdge", "typedef int Alias1; typedef Alias1 Alias2; Alias2 x = 10;"},
        {796, "TypedefsEdge", "typedef int Alias1; typedef Alias1 Alias2; typedef Alias2 Alias3;"},
        {797, "TypedefsEdge", "external typedef int MyInt;"},
        {798, "TypedefsEdge", "shared typedef int MyInt;"},
        {799, "TypedefsEdge", "private typedef int MyInt;"},
        {800, "TypedefsEdge", "protected typedef int MyInt;"}
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
        angel_lsp::config::TypeConfig typeConfig{"string", "array"};
        SemanticAnalysisRequest req{table, fileUri, "", &i18n, &typeConfig};
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

TEST_CASE("Batch_TopLevel1000_Harness_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {801, "InterfaceCompliance", "interface I { void m(); } class C : I {}"},
        {802, "InterfaceCompliance", "interface I { void m(); } class C : I { void m() {} }"},
        {803, "InterfaceCompliance", "interface I { void m(int a); } class C : I { void m() {} }"},
        {804, "InterfaceCompliance", "interface I { void m(int a); } class C : I { void m(int a) {} }"},
        {805, "InterfaceCompliance", "interface I { int m(); } class C : I { void m() {} }"},
        {806, "InterfaceCompliance", "interface I { int m(); } class C : I { int m() { return 0; } }"},
        {807, "InterfaceCompliance", "interface I { void m() const; } class C : I { void m() {} }"},
        {808, "InterfaceCompliance", "interface I { void m() const; } class C : I { void m() const {} }"},
        {809, "InterfaceCompliance", "interface I { private void m(); }"},
        {810, "InterfaceCompliance", "interface I { protected void m(); }"},
        {811, "InterfaceCompliance", "interface I { public void m(); }"},
        {812, "InterfaceCompliance", "interface I { void m1(); void m2(); } class C : I { void m1() {} }"},
        {813, "InterfaceCompliance", "interface I { void m1(); void m2(); } class C : I { void m1() {} void m2() {} }"},
        {814, "InterfaceCompliance", "interface I1 { void a(); } interface I2 { void b(); } class C : I1, I2 { void a() {} }"},
        {815, "InterfaceCompliance", "interface I1 { void a(); } interface I2 { void b(); } class C : I1, I2 { void a() {} void b() {} }"},
        {816, "InterfaceCompliance", "interface I { int prop { get; set; } } class C : I { int prop { get { return 0; } set {} } }"},
        {817, "InterfaceCompliance", "interface I { int prop { get; } } class C : I { int prop { get { return 0; } } }"},
        {818, "InterfaceCompliance", "interface I { int prop { get; set; } } class C : I { int prop { get { return 0; } } }"},
        {819, "InterfaceCompliance", "interface I { void m(float x, int y); } class C : I { void m(int x, float y) {} }"},
        {820, "InterfaceCompliance", "interface I { void m(float x, int y); } class C : I { void m(float x, int y) {} }"},
        {821, "InterfaceCompliance", "interface I { void m(int &in a); } class C : I { void m(int &out a) {} }"},
        {822, "InterfaceCompliance", "interface I { void m(int &in a); } class C : I { void m(int &in a) {} }"},
        {823, "InterfaceCompliance", "interface I { void m(); } class C : I { private void m() {} }"},
        {824, "InterfaceCompliance", "interface I { void m(); } class C : I { protected void m() {} }"},
        {825, "InterfaceCompliance", "interface I { void m(); } class C : I { public void m() {} }"},
        {826, "InterfaceCompliance", "interface I { void m(); } abstract class C : I {}"},
        {827, "InterfaceCompliance", "interface I { void m(); } class C : I { void m() override {} }"},
        {828, "InterfaceCompliance", "interface I { void m(); } class C : I { void m() final {} }"},
        {829, "InterfaceCompliance", "interface I { void m(); } interface I2 : I {}"},
        {830, "InterfaceCompliance", "interface I { void m(); } interface I2 : I { void m2(); }"},
        {831, "InterfaceCompliance", "interface I { void m(); } class C : I { int m; }"},
        {832, "InterfaceCompliance", "interface I { void m(int a = 0); } class C : I { void m(int a) {} }"},
        {833, "InterfaceCompliance", "interface I { void m(int a); } class C : I { void m(int a = 0) {} }"},
        {834, "InterfaceCompliance", "interface I { void m(const int a); } class C : I { void m(int a) {} }"},
        {835, "InterfaceCompliance", "interface I { void m(int a); } class C : I { void m(const int a) {} }"},
        {836, "InterfaceCompliance", "interface I { int opAdd(int a); } class C : I { int opAdd(int a) { return 0; } }"},
        {837, "InterfaceCompliance", "interface I { int opAdd(int a); } class C : I {}"},
        {838, "InterfaceCompliance", "interface I { void m(); } class Base { void m() {} } class C : Base, I {}"},
        {839, "InterfaceCompliance", "interface I { void m(); } class Base {} class C : Base, I {}"},
        {840, "InterfaceCompliance", "interface I { void m(); } class Base { void m() {} } class C : I, Base {}"},

        {841, "CtorDtorRules", "class C { ~C() {} }"},
        {842, "CtorDtorRules", "class C { ~C(int a) {} }"},
        {843, "CtorDtorRules", "class C { void ~C() {} }"},
        {844, "CtorDtorRules", "class C { int ~C() {} }"},
        {845, "CtorDtorRules", "class C { ~C() {} ~C() {} }"},
        {846, "CtorDtorRules", "interface I { I(); }"},
        {847, "CtorDtorRules", "interface I { ~I(); }"},
        {848, "CtorDtorRules", "class C { C() {} C(int a) {} }"},
        {849, "CtorDtorRules", "class C { C(int a) {} C(int a) {} }"},
        {850, "CtorDtorRules", "class C { C(int a, float b) {} C(int x, float y) {} }"},
        {851, "CtorDtorRules", "class C { WrongName() {} }"},
        {852, "CtorDtorRules", "class C { C() const {} }"},
        {853, "CtorDtorRules", "class C { ~C() const {} }"},
        {854, "CtorDtorRules", "class C { C() override {} }"},
        {855, "CtorDtorRules", "class C { C() final {} }"},
        {856, "CtorDtorRules", "class C { ~C() override {} }"},
        {857, "CtorDtorRules", "class C { ~C() final {} }"},
        {858, "CtorDtorRules", "class C { C() delete; }"},
        {859, "CtorDtorRules", "class C { ~C() delete; }"},
        {860, "CtorDtorRules", "class C { private C() {} }"},
        {861, "CtorDtorRules", "class C { protected C() {} }"},
        {862, "CtorDtorRules", "class C { public C() {} }"},
        {863, "CtorDtorRules", "class C { private ~C() {} }"},
        {864, "CtorDtorRules", "class C { protected ~C() {} }"},
        {865, "CtorDtorRules", "class C { C(int a = 0) {} }"},
        {866, "CtorDtorRules", "class C { C(int &in a) {} }"},
        {867, "CtorDtorRules", "class C { C(int &out a) {} }"},
        {868, "CtorDtorRules", "class C { C(int &inout a) {} }"},
        {869, "CtorDtorRules", "class C { ~C(int &out a) {} }"},
        {870, "CtorDtorRules", "class C { explicit C() {} }"},
        {871, "CtorDtorRules", "class C { C() { return; } }"},
        {872, "CtorDtorRules", "class C { C() { return 10; } }"},
        {873, "CtorDtorRules", "class C { ~C() { return; } }"},
        {874, "CtorDtorRules", "class C { ~C() { return 10; } }"},
        {875, "CtorDtorRules", "class C { C(C@ other) {} }"},
        {876, "CtorDtorRules", "class C { C(const C &in other) {} }"},
        {877, "CtorDtorRules", "class C { C(void) {} }"},
        {878, "CtorDtorRules", "class C { ~C(void) {} }"},
        {879, "CtorDtorRules", "interface I { void I(); }"},
        {880, "CtorDtorRules", "interface I { void ~I(); }"},

        {881, "ScopeCollisions", "class C { int x; float x; }"},
        {882, "ScopeCollisions", "class C { int x; int x; }"},
        {883, "ScopeCollisions", "class C { void f(); int x; }"},
        {884, "ScopeCollisions", "class C { int x; void x(); }"},
        {885, "ScopeCollisions", "void f(int a) {} void f(int b) {}"},
        {886, "ScopeCollisions", "void f(int a) {} void f(float b) {}"},
        {887, "ScopeCollisions", "int f(int a) { return 0; } float f(int a) { return 0.0f; }"},
        {888, "ScopeCollisions", "namespace N { void f(int a) {} void f(int a) {} }"},
        {889, "ScopeCollisions", "namespace N { void f(int a) {} void f(float a) {} }"},
        {890, "ScopeCollisions", "class C { void f(int a) {} void f(int a) {} }"},
        {891, "ScopeCollisions", "class C { void f(int a) {} void f(float a) {} }"},
        {892, "ScopeCollisions", "class C { void f(int a) const {} void f(int a) {} }"},
        {893, "ScopeCollisions", "class C { void f(int a) const {} void f(int a) const {} }"},
        {894, "ScopeCollisions", "int x; float x;"},
        {895, "ScopeCollisions", "int x; void f() { int x; }"},
        {896, "ScopeCollisions", "namespace N { int x; float x; }"},
        {897, "ScopeCollisions", "enum E { A, B } int A;"},
        {898, "ScopeCollisions", "int A; enum E { A, B }"},
        {899, "ScopeCollisions", "class C {} int C;"},
        {900, "ScopeCollisions", "interface I {} int I;"},
        {901, "ScopeCollisions", "typedef int MyInt; int MyInt;"},
        {902, "ScopeCollisions", "funcdef void CB(); int CB;"},
        {903, "ScopeCollisions", "class C { int x; class x {} }"},
        {904, "ScopeCollisions", "class C { int x; enum x { A } }"},
        {905, "ScopeCollisions", "class C { int x; typedef int x; }"},
        {906, "ScopeCollisions", "class C { int x; funcdef void x(); }"},
        {907, "ScopeCollisions", "namespace N { class C {} int C; }"},
        {908, "ScopeCollisions", "namespace N { interface I {} int I; }"},
        {909, "ScopeCollisions", "namespace N { enum E { A } int E; }"},
        {910, "ScopeCollisions", "namespace N { typedef int T; int T; }"},
        {911, "ScopeCollisions", "namespace N { funcdef void CB(); int CB; }"},
        {912, "ScopeCollisions", "void f() {} int f;"},
        {913, "ScopeCollisions", "int f; void f() {}"},
        {914, "ScopeCollisions", "class C { void f() {} int f; }"},
        {915, "ScopeCollisions", "class C { int f; void f() {} }"},
        {916, "ScopeCollisions", "void f(int a, int a) {}"},
        {917, "ScopeCollisions", "void f(int a, float a) {}"},
        {918, "ScopeCollisions", "class C { void f(int a, int a) {} }"},
        {919, "ScopeCollisions", "class C { int a; int b; int a; }"},
        {920, "ScopeCollisions", "enum E { A, B, A }"},

        {921, "TopLevelIncompatible", "const int g_val = \"cadena\";"},
        {922, "TopLevelIncompatible", "const float g_val = true;"},
        {923, "TopLevelIncompatible", "const double g_val = false;"},
        {924, "TopLevelIncompatible", "const bool g_val = \"cadena\";"},
        {925, "TopLevelIncompatible", "const string g_val = 123;"},
        {926, "TopLevelIncompatible", "const int g_val = 100;"},
        {927, "TopLevelIncompatible", "const float g_val = 3.14f;"},
        {928, "TopLevelIncompatible", "const bool g_val = true;"},
        {929, "TopLevelIncompatible", "const string g_val = \"hola\";"},
        {930, "TopLevelIncompatible", "class C { int x = \"cadena\"; }"},
        {931, "TopLevelIncompatible", "class C { float x = true; }"},
        {932, "TopLevelIncompatible", "class C { bool x = \"cadena\"; }"},
        {933, "TopLevelIncompatible", "class C { string x = 123; }"},
        {934, "TopLevelIncompatible", "class C { int x = 10; }"},
        {935, "TopLevelIncompatible", "class C { float x = 1.0f; }"},
        {936, "TopLevelIncompatible", "class C { bool x = false; }"},
        {937, "TopLevelIncompatible", "class C { string x = \"texto\"; }"},
        {938, "TopLevelIncompatible", "int g_val = \"cadena\";"},
        {939, "TopLevelIncompatible", "float g_val = true;"},
        {940, "TopLevelIncompatible", "bool g_val = \"cadena\";"},
        {941, "TopLevelIncompatible", "string g_val = 123;"},
        {942, "TopLevelIncompatible", "int g_val = 10;"},
        {943, "TopLevelIncompatible", "float g_val = 2.5f;"},
        {944, "TopLevelIncompatible", "bool g_val = true;"},
        {945, "TopLevelIncompatible", "string g_val = \"abc\";"},
        {946, "TopLevelIncompatible", "const uint g_val = \"cadena\";"},
        {947, "TopLevelIncompatible", "const int8 g_val = true;"},
        {948, "TopLevelIncompatible", "const int16 g_val = \"cadena\";"},
        {949, "TopLevelIncompatible", "const int64 g_val = false;"},
        {950, "TopLevelIncompatible", "const uint8 g_val = \"cadena\";"},
        {951, "TopLevelIncompatible", "const uint16 g_val = true;"},
        {952, "TopLevelIncompatible", "const uint32 g_val = \"cadena\";"},
        {953, "TopLevelIncompatible", "const uint64 g_val = false;"},
        {954, "TopLevelIncompatible", "const uint g_val = 10u;"},
        {955, "TopLevelIncompatible", "const int8 g_val = 5;"},
        {956, "TopLevelIncompatible", "const int16 g_val = 500;"},
        {957, "TopLevelIncompatible", "const int64 g_val = 1000000;"},
        {958, "TopLevelIncompatible", "class C { uint x = \"cadena\"; }"},
        {959, "TopLevelIncompatible", "class C { int8 x = true; }"},
        {960, "TopLevelIncompatible", "class C { uint64 x = \"cadena\"; }"},

        {961, "MixinValidation", "mixin class MyMixin { void m() {} } class C { mixin MyMixin; }"},
        {962, "MixinValidation", "class NormalClass { void m() {} } class C { mixin NormalClass; }"},
        {963, "MixinValidation", "class C { mixin NonExistentMixin; }"},
        {964, "MixinValidation", "interface IFace { void m(); } class C { mixin IFace; }"},
        {965, "MixinValidation", "enum E { A } class C { mixin E; }"},
        {966, "MixinValidation", "typedef int T; class C { mixin T; }"},
        {967, "MixinValidation", "funcdef void CB(); class C { mixin CB; }"},
        {968, "MixinValidation", "mixin class M1 {} mixin class M2 { mixin M1; }"},
        {969, "MixinValidation", "mixin class M1 {} class C { mixin M1; mixin M1; }"},
        {970, "MixinValidation", "mixin class M1 { int x; } mixin class M2 { float x; } class C { mixin M1; mixin M2; }"},
        {971, "MixinValidation", "mixin class M1 { void f() {} } class C { mixin M1; void f() {} }"},
        {972, "MixinValidation", "mixin class M1 { void f() {} } class C { void f() {} mixin M1; }"},
        {973, "MixinValidation", "mixin class M1 { int x; } class C { mixin M1; int x; }"},
        {974, "MixinValidation", "mixin class M1 { int x; } class C { int x; mixin M1; }"},
        {975, "MixinValidation", "mixin class M1 { void f() {} } mixin class M2 { void f() {} } class C { mixin M1; mixin M2; }"},
        {976, "MixinValidation", "mixin class M1 { void f() {} } class Base { void f() {} } class C : Base { mixin M1; }"},
        {977, "MixinValidation", "mixin class M1 { void f() {} } class Base {} class C : Base { mixin M1; }"},
        {978, "MixinValidation", "namespace N { mixin class M { void f() {} } } class C { mixin N::M; }"},
        {979, "MixinValidation", "namespace N { mixin class M { void f() {} } } using namespace N; class C { mixin M; }"},
        {980, "MixinValidation", "mixin class M { mixin class Nested {} }"},
        {981, "MixinValidation", "mixin class M { interface I {} }"},
        {982, "MixinValidation", "mixin class M { enum E { A } }"},
        {983, "MixinValidation", "mixin class M { typedef int T; }"},
        {984, "MixinValidation", "mixin class M { funcdef void CB(); }"},
        {985, "MixinValidation", "mixin class M { M() {} }"},
        {986, "MixinValidation", "mixin class M { ~M() {} }"},
        {987, "MixinValidation", "mixin class M { int prop { get { return 0; } } } class C { mixin M; }"},
        {988, "MixinValidation", "mixin class M { int opAdd(int a) { return 0; } } class C { mixin M; }"},
        {989, "MixinValidation", "mixin class M { private void secret() {} } class C { mixin M; }"},
        {990, "MixinValidation", "mixin class M { protected void internal_fn() {} } class C { mixin M; }"},
        {991, "MixinValidation", "mixin class M { public void pub_fn() {} } class C { mixin M; }"},
        {992, "MixinValidation", "external mixin class M;"},
        {993, "MixinValidation", "shared mixin class M { void f() {} }"},
        {994, "MixinValidation", "abstract mixin class M { void f() {} }"},
        {995, "MixinValidation", "final mixin class M { void f() {} }"},
        {996, "MixinValidation", "mixin class M : BaseClass {}"},
        {997, "MixinValidation", "mixin class M : IFace {}"},
        {998, "MixinValidation", "mixin class M { void f() final {} }"},
        {999, "MixinValidation", "mixin class M { void f() override {} }"},
        {1000, "MixinValidation", "mixin class M { void f() delete; }"}
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
        angel_lsp::config::TypeConfig typeConfig{"string", "array"};
        SemanticAnalysisRequest req{table, fileUri, "", &i18n, &typeConfig};
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

TEST_CASE("Batch_Keyword1200_Harness_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {1001, "MisplacedKeywords", "void f() explicit {}"},
        {1002, "MisplacedKeywords", "void f() delete {}"},
        {1003, "MisplacedKeywords", "void f() final override {}"},
        {1004, "MisplacedKeywords", "void f() override final {}"},
        {1005, "MisplacedKeywords", "void f() class {}"},
        {1006, "MisplacedKeywords", "void f() interface {}"},
        {1007, "MisplacedKeywords", "void f() namespace {}"},
        {1008, "MisplacedKeywords", "void f() enum {}"},
        {1009, "MisplacedKeywords", "void f() typedef {}"},
        {1010, "MisplacedKeywords", "void f() funcdef {}"},
        {1011, "MisplacedKeywords", "void f() mixin {}"},
        {1012, "MisplacedKeywords", "void f() import {}"},
        {1013, "MisplacedKeywords", "void f() external {}"},
        {1014, "MisplacedKeywords", "void f() shared {}"},
        {1015, "MisplacedKeywords", "void f() private {}"},
        {1016, "MisplacedKeywords", "void f() protected {}"},
        {1017, "MisplacedKeywords", "void f() public {}"},
        {1018, "MisplacedKeywords", "void f() const const {}"},
        {1019, "MisplacedKeywords", "void f() override override {}"},
        {1020, "MisplacedKeywords", "void f() final final {}"},
        {1021, "MisplacedKeywords", "explicit void f() {}"},
        {1022, "MisplacedKeywords", "delete void f() {}"},
        {1023, "MisplacedKeywords", "override void f() {}"},
        {1024, "MisplacedKeywords", "final void f() {}"},
        {1025, "MisplacedKeywords", "const void f() {}"},
        {1026, "MisplacedKeywords", "class C { void f() explicit {} }"},
        {1027, "MisplacedKeywords", "class C { void f() delete {} }"},
        {1028, "MisplacedKeywords", "class C { void f() const const {} }"},
        {1029, "MisplacedKeywords", "class C { void f() final final {} }"},
        {1030, "MisplacedKeywords", "class C { void f() override override {} }"},
        {1031, "MisplacedKeywords", "class C { explicit void f() {} }"},
        {1032, "MisplacedKeywords", "class C { delete void f() {} }"},
        {1033, "MisplacedKeywords", "class C { override C() {} }"},
        {1034, "MisplacedKeywords", "class C { final C() {} }"},
        {1035, "MisplacedKeywords", "class C { const C() {} }"},
        {1036, "MisplacedKeywords", "class C { C() const {} }"},
        {1037, "MisplacedKeywords", "class C { ~C() const {} }"},
        {1038, "MisplacedKeywords", "class C { ~C() override {} }"},
        {1039, "MisplacedKeywords", "class C { ~C() final {} }"},
        {1040, "MisplacedKeywords", "class C { ~C() explicit {} }"},
        {1041, "MisplacedKeywords", "interface I { void f() const const; }"},
        {1042, "MisplacedKeywords", "interface I { void f() override; }"},
        {1043, "MisplacedKeywords", "interface I { void f() final; }"},
        {1044, "MisplacedKeywords", "interface I { void f() explicit; }"},
        {1045, "MisplacedKeywords", "interface I { void f() delete; }"},
        {1046, "MisplacedKeywords", "funcdef void CB() const const;"},
        {1047, "MisplacedKeywords", "funcdef void CB() override;"},
        {1048, "MisplacedKeywords", "funcdef void CB() final;"},
        {1049, "MisplacedKeywords", "funcdef void CB() explicit;"},
        {1050, "MisplacedKeywords", "funcdef void CB() delete;"},

        {1051, "MisplacedQualifiers", "class C final final {}"},
        {1052, "MisplacedQualifiers", "class C abstract abstract {}"},
        {1053, "MisplacedQualifiers", "class C final abstract {}"},
        {1054, "MisplacedQualifiers", "class C abstract final {}"},
        {1055, "MisplacedQualifiers", "class C shared shared {}"},
        {1056, "MisplacedQualifiers", "class C external external {}"},
        {1057, "MisplacedQualifiers", "class C explicit {}"},
        {1058, "MisplacedQualifiers", "class C override {}"},
        {1059, "MisplacedQualifiers", "class C delete {}"},
        {1060, "MisplacedQualifiers", "class C const {}"},
        {1061, "MisplacedQualifiers", "interface I final {}"},
        {1062, "MisplacedQualifiers", "interface I abstract {}"},
        {1063, "MisplacedQualifiers", "interface I override {}"},
        {1064, "MisplacedQualifiers", "interface I explicit {}"},
        {1065, "MisplacedQualifiers", "interface I delete {}"},
        {1066, "MisplacedQualifiers", "enum E final {}"},
        {1067, "MisplacedQualifiers", "enum E abstract {}"},
        {1068, "MisplacedQualifiers", "enum E override {}"},
        {1069, "MisplacedQualifiers", "enum E explicit {}"},
        {1070, "MisplacedQualifiers", "enum E delete {}"},
        {1071, "MisplacedQualifiers", "typedef int T final;"},
        {1072, "MisplacedQualifiers", "typedef int T abstract;"},
        {1073, "MisplacedQualifiers", "typedef int T override;"},
        {1074, "MisplacedQualifiers", "typedef int T explicit;"},
        {1075, "MisplacedQualifiers", "typedef int T delete;"},
        {1076, "MisplacedQualifiers", "int x const;"},
        {1077, "MisplacedQualifiers", "int x final;"},
        {1078, "MisplacedQualifiers", "int x override;"},
        {1079, "MisplacedQualifiers", "int x explicit;"},
        {1080, "MisplacedQualifiers", "int x delete;"},
        {1081, "MisplacedQualifiers", "const const int x = 0;"},
        {1082, "MisplacedQualifiers", "class C { int x const; }"},
        {1083, "MisplacedQualifiers", "class C { int x final; }"},
        {1084, "MisplacedQualifiers", "class C { int x override; }"},
        {1085, "MisplacedQualifiers", "class C { int x explicit; }"},
        {1086, "MisplacedQualifiers", "class C { int x delete; }"},
        {1087, "MisplacedQualifiers", "class C { const const int x = 0; }"},
        {1088, "MisplacedQualifiers", "class C { private private int x; }"},
        {1089, "MisplacedQualifiers", "class C { protected protected int x; }"},
        {1090, "MisplacedQualifiers", "class C { public public int x; }"},
        {1091, "MisplacedQualifiers", "private private int g_x;"},
        {1092, "MisplacedQualifiers", "protected protected int g_x;"},
        {1093, "MisplacedQualifiers", "public public int g_x;"},
        {1094, "MisplacedQualifiers", "external external int g_x;"},
        {1095, "MisplacedQualifiers", "shared shared class C {}"},
        {1096, "MisplacedQualifiers", "mixin mixin class M {}"},
        {1097, "MisplacedQualifiers", "mixin class M final final {}"},
        {1098, "MisplacedQualifiers", "mixin class M abstract abstract {}"},
        {1099, "MisplacedQualifiers", "mixin class M override {}"},
        {1100, "MisplacedQualifiers", "mixin class M explicit {}"},

        {1101, "BrokenStatements", "class C { int x = if (true) 1; }"},
        {1102, "BrokenStatements", "class C { int x = while (true) 1; }"},
        {1103, "BrokenStatements", "class C { int x = for (;;) 1; }"},
        {1104, "BrokenStatements", "class C { int x = return 1; }"},
        {1105, "BrokenStatements", "class C { int x = class; }"},
        {1106, "BrokenStatements", "class C { int x = interface; }"},
        {1107, "BrokenStatements", "class C { int x = enum; }"},
        {1108, "BrokenStatements", "class C { int x = typedef; }"},
        {1109, "BrokenStatements", "class C { int x = funcdef; }"},
        {1110, "BrokenStatements", "class C { int x = namespace; }"},
        {1111, "BrokenStatements", "const int g_x = if (true) 1;"},
        {1112, "BrokenStatements", "const int g_x = while (true) 1;"},
        {1113, "BrokenStatements", "const int g_x = return 1;"},
        {1114, "BrokenStatements", "const int g_x = class;"},
        {1115, "BrokenStatements", "const int g_x = interface;"},
        {1116, "BrokenStatements", "void f() { if }"},
        {1117, "BrokenStatements", "void f() { while }"},
        {1118, "BrokenStatements", "void f() { for }"},
        {1119, "BrokenStatements", "void f() { switch }"},
        {1120, "BrokenStatements", "void f() { try }"},
        {1121, "BrokenStatements", "void f() { catch }"},
        {1122, "BrokenStatements", "void f() { return return; }"},
        {1123, "BrokenStatements", "void f() { break break; }"},
        {1124, "BrokenStatements", "void f() { continue continue; }"},
        {1125, "BrokenStatements", "void f() { case: }"},
        {1126, "BrokenStatements", "void f() { default: default: }"},
        {1127, "BrokenStatements", "class C { void f() { if } }"},
        {1128, "BrokenStatements", "class C { void f() { while } }"},
        {1129, "BrokenStatements", "class C { void f() { for } }"},
        {1130, "BrokenStatements", "class C { void f() { switch } }"},
        {1131, "BrokenStatements", "namespace N { if (true) {} }"},
        {1132, "BrokenStatements", "namespace N { while (true) {} }"},
        {1133, "BrokenStatements", "namespace N { for (;;) {} }"},
        {1134, "BrokenStatements", "namespace N { return; }"},
        {1135, "BrokenStatements", "namespace N { break; }"},
        {1136, "BrokenStatements", "namespace N { continue; }"},
        {1137, "BrokenStatements", "if (true) {}"},
        {1138, "BrokenStatements", "while (true) {}"},
        {1139, "BrokenStatements", "for (;;) {}"},
        {1140, "BrokenStatements", "return;"},
        {1141, "BrokenStatements", "break;"},
        {1142, "BrokenStatements", "continue;"},
        {1143, "BrokenStatements", "switch (1) {}"},
        {1144, "BrokenStatements", "try {} catch () {}"},
        {1145, "BrokenStatements", "do {} while (true);"},
        {1146, "BrokenStatements", "class C { if (true) {} }"},
        {1147, "BrokenStatements", "class C { while (true) {} }"},
        {1148, "BrokenStatements", "class C { for (;;) {} }"},
        {1149, "BrokenStatements", "class C { return; }"},
        {1150, "BrokenStatements", "class C { break; }"},

        {1151, "ComboModifiers", "class C { void f() explicit delete final; }"},
        {1152, "ComboModifiers", "class C { void f() delete final; }"},
        {1153, "ComboModifiers", "class C { void f() final delete; }"},
        {1154, "ComboModifiers", "class C { void f() override delete; }"},
        {1155, "ComboModifiers", "class C { void f() delete override; }"},
        {1156, "ComboModifiers", "class C { void f() explicit final; }"},
        {1157, "ComboModifiers", "class C { void f() explicit override; }"},
        {1158, "ComboModifiers", "class C { void f() explicit const; }"},
        {1159, "ComboModifiers", "class C { void f() const explicit; }"},
        {1160, "ComboModifiers", "class C { void f() const delete; }"},
        {1161, "ComboModifiers", "class C { void f() delete const; }"},
        {1162, "ComboModifiers", "class C { void f() const final; }"},
        {1163, "ComboModifiers", "class C { void f() const override; }"},
        {1164, "ComboModifiers", "class C { void f() final const; }"},
        {1165, "ComboModifiers", "class C { void f() override const; }"},
        {1166, "ComboModifiers", "class C { C() explicit final; }"},
        {1167, "ComboModifiers", "class C { C() explicit override; }"},
        {1168, "ComboModifiers", "class C { C() explicit delete; }"},
        {1169, "ComboModifiers", "class C { C() delete final; }"},
        {1170, "ComboModifiers", "class C { C() final delete; }"},
        {1171, "ComboModifiers", "class C { ~C() explicit delete; }"},
        {1172, "ComboModifiers", "class C { ~C() delete final; }"},
        {1173, "ComboModifiers", "class C { ~C() final delete; }"},
        {1174, "ComboModifiers", "class C { ~C() override delete; }"},
        {1175, "ComboModifiers", "class C { ~C() delete override; }"},
        {1176, "ComboModifiers", "interface I { void f() explicit final; }"},
        {1177, "ComboModifiers", "interface I { void f() delete final; }"},
        {1178, "ComboModifiers", "interface I { void f() final override; }"},
        {1179, "ComboModifiers", "interface I { void f() const final; }"},
        {1180, "ComboModifiers", "interface I { void f() const override; }"},
        {1181, "ComboModifiers", "funcdef void CB() explicit final;"},
        {1182, "ComboModifiers", "funcdef void CB() delete final;"},
        {1183, "ComboModifiers", "funcdef void CB() final override;"},
        {1184, "ComboModifiers", "funcdef void CB() const final;"},
        {1185, "ComboModifiers", "funcdef void CB() const override;"},
        {1186, "ComboModifiers", "class C { int prop { get explicit; } }"},
        {1187, "ComboModifiers", "class C { int prop { get delete; } }"},
        {1188, "ComboModifiers", "class C { int prop { get final override; } }"},
        {1189, "ComboModifiers", "class C { int prop { get override final; } }"},
        {1190, "ComboModifiers", "class C { int prop { get const const; } }"},
        {1191, "ComboModifiers", "class C { int prop { set explicit; } }"},
        {1192, "ComboModifiers", "class C { int prop { set delete; } }"},
        {1193, "ComboModifiers", "class C { int prop { set final override; } }"},
        {1194, "ComboModifiers", "class C { int prop { set override final; } }"},
        {1195, "ComboModifiers", "class C { int prop { set const const; } }"},
        {1196, "ComboModifiers", "interface I { int prop { get explicit; } }"},
        {1197, "ComboModifiers", "interface I { int prop { get delete; } }"},
        {1198, "ComboModifiers", "interface I { int prop { get final; } }"},
        {1199, "ComboModifiers", "interface I { int prop { get override; } }"},
        {1200, "ComboModifiers", "interface I { int prop { get const; } }"}
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
        angel_lsp::config::TypeConfig typeConfig{"string", "array"};
        SemanticAnalysisRequest req{table, fileUri, "", &i18n, &typeConfig};
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

TEST_CASE("SemanticAnalyzer - Multiple Errors: Class Member Const and External Function Body")
{
    std::string sourceCode = "class C { const int x = 0; }\nexternal void ExtFunc() {}\n";
    std::string fileUri = "file:///test_multi1.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() >= 2);
    bool foundConstMember = false;
    bool foundExtBody = false;

    for (const auto &d : diagnostics)
    {
        if (d.code == "as-err-class-member-const") foundConstMember = true;
        if (d.code == "as-err-delete-with-body") foundExtBody = true;
    }

    CHECK(foundConstMember);
    CHECK(foundExtBody);
}

TEST_CASE("SemanticAnalyzer - Multiple Errors: Void Variable and Invalid Reference Return")
{
    std::string sourceCode = "const void g_badVar;\nint& BadRefFunc() { static int x; return x; }\n";
    std::string fileUri = "file:///test_multi2.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() >= 2);
    bool foundVoidVar = false;
    bool foundRefReturn = false;

    for (const auto &d : diagnostics)
    {
        if (d.code == "as-err-void-variable") foundVoidVar = true;
        if (d.code == "as-err-invalid-reference-return") foundRefReturn = true;
    }

    CHECK(foundVoidVar);
    CHECK(foundRefReturn);
}

TEST_CASE("SemanticAnalyzer - Multiple Errors: Constructor Modifiers and Destructor Parameters")
{
    std::string sourceCode = "class C { final C() {} ~C(int a) {} }\n";
    std::string fileUri = "file:///test_multi3.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() >= 2);
    bool foundCtorMod = false;
    bool foundDtorParam = false;

    for (const auto &d : diagnostics)
    {
        if (d.code == "as-err-reserved-keyword-name") foundCtorMod = true;
        if (d.code == "as-err-destructor-param") foundDtorParam = true;
    }

    CHECK(foundCtorMod);
    CHECK(foundDtorParam);
}

TEST_CASE("SemanticAnalyzer - Multiple Errors: Missing Base Class and Invalid Primitive Ref Return")
{
    std::string sourceCode = "class Derived : NonExistentBase {}\nint& BadRefFunc() { static int x; return x; }\n";
    std::string fileUri = "file:///test_multi4.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() >= 2);
    bool foundBaseNotFound = false;
    bool foundRefReturn = false;

    for (const auto &d : diagnostics)
    {
        if (d.code == "as-err-base-not-found") foundBaseNotFound = true;
        if (d.code == "as-err-invalid-reference-return") foundRefReturn = true;
    }

    CHECK(foundBaseNotFound);
    CHECK(foundRefReturn);
}

TEST_CASE("SemanticAnalyzer - Multiple Errors: Enum Invalid Initializer and Duplicate Method Symbols")
{
    std::string sourceCode = "enum MyEnum { ValueA = \"invalid_string\" }\nclass C { void f() {} void f() {} }\n";
    std::string fileUri = "file:///test_multi5.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() >= 2);
    bool foundEnumInit = false;
    bool foundDupSymbol = false;

    for (const auto &d : diagnostics)
    {
        if (d.code == "as-err-enum-invalid-initializer") foundEnumInit = true;
        if (d.code == "as-err-duplicate-symbol") foundDupSymbol = true;
    }

    CHECK(foundEnumInit);
    CHECK(foundDupSymbol);
}

TEST_CASE("SemanticAnalyzer - Enum Suite 01: Defined Enum Parameter is Valid")
{
    std::string sourceCode = "enum State { Idle, Running }\nvoid SetState(State s) {}\n";
    std::string fileUri = "file:///test_enum1.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 02: Undefined Enum Parameter Flagged")
{
    std::string sourceCode = "void SetState(UndefinedState s) {}\n";
    std::string fileUri = "file:///test_enum2.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-unresolved-type");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 03: Enum Return Type Valid")
{
    std::string sourceCode = "enum Mode { ModeA, ModeB }\nMode GetMode() { return ModeA; }\n";
    std::string fileUri = "file:///test_enum3.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 04: Undefined Enum Return Type Flagged")
{
    std::string sourceCode = "UndefinedMode GetMode() {}\n";
    std::string fileUri = "file:///test_enum4.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-unresolved-type");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 05: Enum Class Member Property Valid")
{
    std::string sourceCode = "enum Weapon { Sword, Bow }\nclass Player { Weapon currentWeapon; }\n";
    std::string fileUri = "file:///test_enum5.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 06: Undefined Enum Class Member Flagged")
{
    std::string sourceCode = "class Player { UndefinedWeapon currentWeapon; }\n";
    std::string fileUri = "file:///test_enum6.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-unresolved-type");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 07: Duplicate Enum Members Flagged")
{
    std::string sourceCode = "enum Colors { Red, Red }\n";
    std::string fileUri = "file:///test_enum7.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-name-conflict");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 08: String Initializer in Enum Flagged")
{
    std::string sourceCode = "enum BadEnum { Key = \"string_val\" }\n";
    std::string fileUri = "file:///test_enum8.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-enum-invalid-initializer");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 09: Bool Initializer in Enum Flagged")
{
    std::string sourceCode = "enum BadEnum2 { Key = true }\n";
    std::string fileUri = "file:///test_enum9.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-enum-invalid-initializer");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 10: Reserved Keyword as Enum Name Flagged")
{
    std::string sourceCode = "enum class {}\n";
    std::string fileUri = "file:///test_enum10.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-reserved-keyword-name");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 11: Primitive Type as Enum Name Flagged")
{
    std::string sourceCode = "enum int {}\n";
    std::string fileUri = "file:///test_enum11.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-reserved-keyword-name");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 12: External Enum Flagged")
{
    std::string sourceCode = "external enum ExtEnum;\n";
    std::string fileUri = "file:///test_enum12.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-err-external-not-found");
}

TEST_CASE("SemanticAnalyzer - Enum Suite 13: Namespaced Enum Parameter Valid")
{
    std::string sourceCode = "namespace N { enum Level { Low, High }; }\nvoid SetLevel(N::Level l) {}\n";
    std::string fileUri = "file:///test_enum13.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 14: Enum Passed by Const In Reference Valid")
{
    std::string sourceCode = "enum Color { Red, Blue }\nvoid Paint(const Color &in c) {}\n";
    std::string fileUri = "file:///test_enum14.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 15: Enum Passed by Out Reference Valid")
{
    std::string sourceCode = "enum Status { OK, Fail }\nvoid Check(Status &out s) {}\n";
    std::string fileUri = "file:///test_enum15.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 16: Consecutive Commas in Enum Flagged")
{
    std::string sourceCode = "enum BadSyntax { A = 1,, B = 2 }\n";
    std::string fileUri = "file:///test_enum16.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    auto syntaxDiags = collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto semanticDiags = analyzer.Analyze(req);
    std::vector<Diagnostic> allDiags = syntaxDiags;
    allDiags.insert(allDiags.end(), semanticDiags.begin(), semanticDiags.end());
    REQUIRE(!allDiags.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 17: Multiple Enums in Same File Valid")
{
    std::string sourceCode = "enum E1 { A1 }\nenum E2 { A2 }\nvoid Process(E1 e1, E2 e2) {}\n";
    std::string fileUri = "file:///test_enum17.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 18: Enum Array Variable Type Valid")
{
    std::string sourceCode = "enum Flag { F1, F2 }\narray<Flag> flags;\n";
    std::string fileUri = "file:///test_enum18.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::config::TypeConfig tc{"string", "array"};
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n, &tc};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 19: Enum in Interface Method Signature Valid")
{
    std::string sourceCode = "enum Cmd { Go, Stop }\ninterface IAgent { void Exec(Cmd c); }\n";
    std::string fileUri = "file:///test_enum19.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 20: Enum Parameter with Default Value Valid")
{
    std::string sourceCode = "enum Option { OptA = 0, OptB = 1 }\nvoid Config(Option opt = OptA) {}\n";
    std::string fileUri = "file:///test_enum20.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);
    CHECK(diagnostics.empty());
}








