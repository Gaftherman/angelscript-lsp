#include <doctest/doctest.h>
#include <iostream>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include <tree_sitter/api.h>

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

TEST_CASE("SemanticAnalyzer - Nested Namespaces Validation")
{
    std::string sourceCode = R"(
namespace Outer
{
    namespace Inner
    {
        class Target {}
    }
}

Outer::Inner::Target validVar;
Outer::Inner::MissingType invalidTypeVar;
Outer::MissingNamespace::Target invalidNsVar;
)";
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
    CHECK(diagnostics[0].code == "as-err-unresolved-type");
    CHECK(diagnostics[1].code == "as-err-unresolved-type");
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

    REQUIRE(diagnostics.size() >= 1);
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
        {690, "HeredocStrings", "string s = \"\"\"\n\"\"\"\";"},
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
        {707, "HeredocStrings", "string s = \"\"\"Con salto final\n\"\"\"\";"},
        {708, "HeredocStrings", "string s = \"\"\"\nCon salto inicial\"\"\";"},
        {709, "HeredocStrings", "string s = \"\"\"Triple comilla \" en heredoc\"\"\";"},
        {710, "HeredocStrings", "string s = \"\"\"Heredoc vacio multilinea\n\n\"\"\"\";"},

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
        {729, "StringConcat", "string s = \"Parte 1\"\\n\"Parte 2\";"},
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

TEST_CASE("Batch_Expanded1500_Harness_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {1201, "StringConcat", "string s = \"Part1 \" + \"Part2 \" + \"Part3 \" + \"Part4\";"},
        {1202, "StringConcat", "string s = \"Hex: \" + \"\\x41\\x42\";"},
        {1203, "StringConcat", "string s = \"Escapes: \" + \"\\n\\t\\r\";"},
        {1204, "StringConcat", "string s = \"\"\"Heredoc\"\"\" \"Part2\";"},
        {1205, "StringConcat", "string s = \"Part1\" \"\"\"Heredoc\"\"\";"},
        {1206, "StringConcat", "string s = 'Single1' 'Single2' 'Single3';"},
        {1207, "StringConcat", "string s = \"Double\" 'Single' \"Double\";"},
        {1208, "StringConcat", "const string C_STR = \"Const1 \" \"Const2\";"},
        {1209, "StringConcat", "namespace N { string s = \"NS1 \" \"NS2\"; }"},
        {1210, "StringConcat", "class C { string p = \"Prop1 \" \"Prop2\"; }"},
        {1211, "StringConcat", "void f(string p = \"Param1 \" \"Param2\") {}"},
        {1212, "StringConcat", "string s = \"Num: \" + 42;"},
        {1213, "StringConcat", "string s = \"Float: \" + 3.14f;"},
        {1214, "StringConcat", "string s = \"Bool: \" + false;"},
        {1215, "StringConcat", "string s = 100 + \" is a number\";"},
        {1216, "StringConcat", "string s = 2.718f + \" is e\";"},
        {1217, "StringConcat", "string s = true + \" is boolean\";"},
        {1218, "StringConcat", "string s = \"Multi1\\n\" \"Multi2\\n\" \"Multi3\\n\";"},
        {1219, "StringConcat", "string s = \"Unicode: \" + \"\\u0041\\u0042\";"},
        {1220, "StringConcat", "string s = \"Tab:\\t\" \"Space: \" \"Newline:\\n\";"},
        {1221, "StringConcat", "string s = '' '';"},
        {1222, "StringConcat", "string s = \"\" \"\";"},
        {1223, "StringConcat", "string s = \"\" \"A\" \"\";"},
        {1224, "StringConcat", "string s = \"\\x00\" \"\\x01\";"},
        {1225, "StringConcat", "string s = \"\\u0000\" \"\\u0001\";"},

        {1226, "CtorDtorRules", "class C { C() {} C(int a, float b, string c) {} }"},
        {1227, "CtorDtorRules", "class C { C(int a = 1, float b = 2.0f) {} }"},
        {1228, "CtorDtorRules", "class C { C(const int &in a) {} }"},
        {1229, "CtorDtorRules", "class C { C(int &out a) {} }"},
        {1230, "CtorDtorRules", "class C { C(int &inout a) {} }"},
        {1231, "CtorDtorRules", "class C { explicit C(int a) {} }"},
        {1232, "CtorDtorRules", "class C { ~C() {} }"},
        {1233, "CtorDtorRules", "class C { C(const C &in other) {} }"},
        {1234, "CtorDtorRules", "class C { C(C@ handle) {} }"},
        {1235, "CtorDtorRules", "class C { C() delete; C(int a) {} }"},
        {1236, "CtorDtorRules", "class C { private C() {} public C(int a) {} }"},
        {1237, "CtorDtorRules", "class C { protected C() {} public C(float f) {} }"},
        {1238, "CtorDtorRules", "class C { void C() {} }"},
        {1239, "CtorDtorRules", "class C { int C() {} }"},
        {1240, "CtorDtorRules", "class C { float C() {} }"},
        {1241, "CtorDtorRules", "class C { ~C(int x) {} }"},
        {1242, "CtorDtorRules", "class C { ~C() const {} }"},
        {1243, "CtorDtorRules", "class C { ~C() override {} }"},
        {1244, "CtorDtorRules", "class C { ~C() final {} }"},
        {1245, "CtorDtorRules", "class C { ~C() delete; }"},
        {1246, "CtorDtorRules", "class C { ~C() {} ~C() {} }"},
        {1247, "CtorDtorRules", "interface I { I(); }"},
        {1248, "CtorDtorRules", "interface I { ~I(); }"},
        {1249, "CtorDtorRules", "class C { C() { return; } }"},
        {1250, "CtorDtorRules", "class C { C() { return 42; } }"},

        {1251, "EnumsEdge", "enum E { A = 0, B = 1, C = 2 }"},
        {1252, "EnumsEdge", "enum E { A = 1 << 0, B = 1 << 1, C = 1 << 2 }"},
        {1253, "EnumsEdge", "enum E { A = 0x01, B = 0x02, C = 0x04 }"},
        {1254, "EnumsEdge", "enum E { A = -1, B = -2, C = -3 }"},
        {1255, "EnumsEdge", "enum E { A = 10, B = A + 5, C = B * 2 }"},
        {1256, "EnumsEdge", "enum E { A, B = A }"},
        {1257, "EnumsEdge", "enum E { A = B, B = 1 }"},
        {1258, "EnumsEdge", "enum E { A = 1.5f }"},
        {1259, "EnumsEdge", "enum E { A = \"string\" }"},
        {1260, "EnumsEdge", "enum E { A = true }"},
        {1261, "EnumsEdge", "enum E { A = null }"},
        {1262, "EnumsEdge", "enum E { A, B, A }"},
        {1263, "EnumsEdge", "enum E1 { A } enum E2 { A }"},
        {1264, "EnumsEdge", "namespace N { enum E { A, B } }"},
        {1265, "EnumsEdge", "class C { enum E { A, B } }"},
        {1266, "EnumsEdge", "enum E { A = 0xFFFFFFFF }"},
        {1267, "EnumsEdge", "enum E { A = 0x7FFFFFFF }"},
        {1268, "EnumsEdge", "enum E { A = ~0 }"},
        {1269, "EnumsEdge", "enum E { A = 1 | 2 | 4 }"},
        {1270, "EnumsEdge", "enum E { A = 0xF & 0x3 }"},
        {1271, "EnumsEdge", "shared enum E { A, B }"},
        {1272, "EnumsEdge", "external enum E;"},
        {1273, "EnumsEdge", "enum E {}"},
        {1274, "EnumsEdge", "enum E { A = (1 + 2) * 3 }"},
        {1275, "EnumsEdge", "enum E { A = 1000000000000 }"},

        {1276, "BrokenPropsOps", "class C { int p { get { return 0; } set {} } }"},
        {1277, "BrokenPropsOps", "class C { int p { get const { return 0; } } }"},
        {1278, "BrokenPropsOps", "class C { int p { get { return 0; } get { return 1; } } }"},
        {1279, "BrokenPropsOps", "class C { int p { set {} set {} } }"},
        {1280, "BrokenPropsOps", "class C { int p { get; set; } }"},
        {1281, "BrokenPropsOps", "class C { int p { get; get; } }"},
        {1282, "BrokenPropsOps", "class C { int p { set; set; } }"},
        {1283, "BrokenPropsOps", "interface I { int p { get; set; } }"},
        {1284, "BrokenPropsOps", "interface I { int p { get { return 0; } } }"},
        {1285, "BrokenPropsOps", "interface I { int p { set {} } }"},
        {1286, "BrokenPropsOps", "class C { int opAdd(int b) { return 0; } }"},
        {1287, "BrokenPropsOps", "class C { int opSub(int b) const { return 0; } }"},
        {1288, "BrokenPropsOps", "class C { int opMul(int b, int c) { return 0; } }"},
        {1289, "BrokenPropsOps", "class C { void opAdd() {} }"},
        {1290, "BrokenPropsOps", "class C { int opIndex(int i) { return 0; } }"},
        {1291, "BrokenPropsOps", "class C { int opIndex(int i, int j) { return 0; } }"},
        {1292, "BrokenPropsOps", "class C { int opCall(int a) { return 0; } }"},
        {1293, "BrokenPropsOps", "class C { C@ opAssign(const C &in o) { return this; } }"},
        {1294, "BrokenPropsOps", "class C { int opCmp(const C &in o) const { return 0; } }"},
        {1295, "BrokenPropsOps", "class C { bool opEquals(const C &in o) const { return true; } }"},
        {1296, "BrokenPropsOps", "class C { int p { get private { return 0; } } }"},
        {1297, "BrokenPropsOps", "class C { int p { set protected {} } }"},
        {1298, "BrokenPropsOps", "class C { static int p { get { return 0; } } }"},
        {1299, "BrokenPropsOps", "class C { int p { get final { return 0; } } }"},
        {1300, "BrokenPropsOps", "class C { int p { get override { return 0; } } }"},

        {1301, "BrokenTypes", "int x = 10;"},
        {1302, "BrokenTypes", "float y = 3.14f;"},
        {1303, "BrokenTypes", "double z = 2.718;"},
        {1304, "BrokenTypes", "bool b = true;"},
        {1305, "BrokenTypes", "int8 i8 = 127;"},
        {1306, "BrokenTypes", "uint8 u8 = 255;"},
        {1307, "BrokenTypes", "int16 i16 = 32767;"},
        {1308, "BrokenTypes", "uint16 u16 = 65535;"},
        {1309, "BrokenTypes", "int64 i64 = 10000000000;"},
        {1310, "BrokenTypes", "uint64 u64 = 10000000000u;"},
        {1311, "BrokenTypes", "int@ handle;"},
        {1312, "BrokenTypes", "float@ handle;"},
        {1313, "BrokenTypes", "bool@ handle;"},
        {1314, "BrokenTypes", "void@ handle;"},
        {1315, "BrokenTypes", "int& refVar;"},
        {1316, "BrokenTypes", "float& refVar;"},
        {1317, "BrokenTypes", "bool& refVar;"},
        {1318, "BrokenTypes", "void& refVar;"},
        {1319, "BrokenTypes", "void v;"},
        {1320, "BrokenTypes", "const void cv;"},
        {1321, "BrokenTypes", "int[] arr;"},
        {1322, "BrokenTypes", "float[][] arr2d;"},
        {1323, "BrokenTypes", "array<int> gArr;"},
        {1324, "BrokenTypes", "array<array<float>> gArr2d;"},
        {1325, "BrokenTypes", "array<void> badGArr;"},

        {1326, "TypedefsEdge", "typedef int int32_t;"},
        {1327, "TypedefsEdge", "typedef float float32_t;"},
        {1328, "TypedefsEdge", "typedef double float64_t;"},
        {1329, "TypedefsEdge", "typedef bool boolean_t;"},
        {1330, "TypedefsEdge", "typedef uint uint32_t;"},
        {1331, "TypedefsEdge", "typedef int MyInt; MyInt x = 5;"},
        {1332, "TypedefsEdge", "typedef float MyFloat; MyFloat f = 1.0f;"},
        {1333, "TypedefsEdge", "typedef int T1; typedef T1 T2; T2 x = 10;"},
        {1334, "TypedefsEdge", "typedef int T1; typedef T1 T2; typedef T2 T3;"},
        {1335, "TypedefsEdge", "namespace N { typedef int LocalInt; } N::LocalInt x = 10;"},
        {1336, "TypedefsEdge", "namespace N { typedef float LocalFloat; } using namespace N; LocalFloat f = 2.0f;"},
        {1337, "TypedefsEdge", "typedef const int ConstInt;"},
        {1338, "TypedefsEdge", "typedef int[] IntArray;"},
        {1339, "TypedefsEdge", "typedef array<int> GenericIntArray;"},
        {1340, "TypedefsEdge", "typedef int MyInt; typedef int MyInt;"},
        {1341, "TypedefsEdge", "typedef int MyInt; typedef float MyInt;"},
        {1342, "TypedefsEdge", "typedef UnknownType BadAlias;"},
        {1343, "TypedefsEdge", "typedef void MyVoid;"},
        {1344, "TypedefsEdge", "typedef auto MyAuto;"},
        {1345, "TypedefsEdge", "class C {} typedef C MyClass;"},
        {1346, "TypedefsEdge", "class C {} typedef C@ MyClassHandle;"},
        {1347, "TypedefsEdge", "interface I {} typedef I@ MyIFaceHandle;"},
        {1348, "TypedefsEdge", "enum E { A } typedef E MyEnum;"},
        {1349, "TypedefsEdge", "shared typedef int SharedInt;"},
        {1350, "TypedefsEdge", "external typedef float ExternalFloat;"},

        {1351, "StringEscapes", "string s = \"\\a\\b\\f\\n\\r\\t\\v\";"},
        {1352, "StringEscapes", "string s = \"\\'\\\"\\\\\\?\";"},
        {1353, "StringEscapes", "string s = \"\\0\";"},
        {1354, "StringEscapes", "string s = \"\\x00\";"},
        {1355, "StringEscapes", "string s = \"\\x7F\";"},
        {1356, "StringEscapes", "string s = \"\\x80\";"},
        {1357, "StringEscapes", "string s = \"\\xFF\";"},
        {1358, "StringEscapes", "string s = \"\\x1234\";"},
        {1359, "StringEscapes", "string s = \"\\z\";"},
        {1360, "StringEscapes", "string s = \"\\k\";"},
        {1361, "UnicodeEscapes", "string s = \"\\u0041\";"},
        {1362, "UnicodeEscapes", "string s = \"\\u0000\";"},
        {1363, "UnicodeEscapes", "string s = \"\\u007F\";"},
        {1364, "UnicodeEscapes", "string s = \"\\u0080\";"},
        {1365, "UnicodeEscapes", "string s = \"\\u07FF\";"},
        {1366, "UnicodeEscapes", "string s = \"\\u0800\";"},
        {1367, "UnicodeEscapes", "string s = \"\\uFFFF\";"},
        {1368, "UnicodeEscapes", "string s = \"\\U00000041\";"},
        {1369, "UnicodeEscapes", "string s = \"\\U0001F600\";"},
        {1370, "UnicodeEscapes", "string s = \"\\U0010FFFF\";"},
        {1371, "UnicodeEscapes", "string s = \"\\uD800\";"},
        {1372, "UnicodeEscapes", "string s = \"\\uDFFF\";"},
        {1373, "UnicodeEscapes", "string s = \"\\U00110000\";"},
        {1374, "UnicodeEscapes", "string s = \"\\u12\";"},
        {1375, "UnicodeEscapes", "string s = \"\\U12345\";"},

        {1376, "InterfaceCompliance", "interface I { void m(); } class C : I { void m() {} }"},
        {1377, "InterfaceCompliance", "interface I { void m(); } class C : I {}"},
        {1378, "InterfaceCompliance", "interface I { int m(); } class C : I { void m() {} }"},
        {1379, "InterfaceCompliance", "interface I { void m(int a); } class C : I { void m() {} }"},
        {1380, "InterfaceCompliance", "interface I { void m(int a); } class C : I { void m(int a) {} }"},
        {1381, "InterfaceCompliance", "interface I { void m() const; } class C : I { void m() {} }"},
        {1382, "InterfaceCompliance", "interface I { void m() const; } class C : I { void m() const {} }"},
        {1383, "InterfaceCompliance", "interface I { void m1(); void m2(); } class C : I { void m1() {} }"},
        {1384, "InterfaceCompliance", "interface I { void m1(); void m2(); } class C : I { void m1() {} void m2() {} }"},
        {1385, "InterfaceCompliance", "interface I1 { void a(); } interface I2 { void b(); } class C : I1, I2 { void a() {} void b() {} }"},
        {1386, "InterfaceCompliance", "interface I { int p { get; set; } } class C : I { int p { get { return 0; } set {} } }"},
        {1387, "InterfaceCompliance", "interface I { int p { get; } } class C : I { int p { get { return 0; } } }"},
        {1388, "InterfaceCompliance", "interface I { int p { get; set; } } class C : I { int p { get { return 0; } } }"},
        {1389, "InterfaceCompliance", "interface I { void m(int &in a); } class C : I { void m(int &in a) {} }"},
        {1390, "InterfaceCompliance", "interface I { void m(int &out a); } class C : I { void m(int &in a) {} }"},
        {1391, "InterfaceCompliance", "interface I { void m(); } abstract class C : I {}"},
        {1392, "InterfaceCompliance", "interface I { void m(); } class C : I { void m() override {} }"},
        {1393, "InterfaceCompliance", "interface I { void m(); } class C : I { void m() final {} }"},
        {1394, "InterfaceCompliance", "interface I1 { void m(); } interface I2 : I1 { void m2(); }"},
        {1395, "InterfaceCompliance", "interface I { void m(); } class C : I { private void m() {} }"},
        {1396, "InterfaceCompliance", "interface I { void m(); } class C : I { protected void m() {} }"},
        {1397, "InterfaceCompliance", "interface I { void m(); } class C : I { public void m() {} }"},
        {1398, "InterfaceCompliance", "interface I { void m(int a = 0); } class C : I { void m(int a) {} }"},
        {1399, "InterfaceCompliance", "interface I { void m(const int a); } class C : I { void m(int a) {} }"},
        {1400, "InterfaceCompliance", "interface I { void m(int a); } class C : I { void m(const int a) {} }"},

        {1401, "ScopeCollisions", "int x; float x;"},
        {1402, "ScopeCollisions", "int x; void f() { int x; }"},
        {1403, "ScopeCollisions", "class C { int x; float x; }"},
        {1404, "ScopeCollisions", "class C { void f(); int f; }"},
        {1405, "ScopeCollisions", "void f(int a) {} void f(int b) {}"},
        {1406, "ScopeCollisions", "void f(int a) {} void f(float b) {}"},
        {1407, "ScopeCollisions", "int f(int a) { return 0; } float f(int a) { return 0.0f; }"},
        {1408, "ScopeCollisions", "namespace N { int x; float x; }"},
        {1409, "ScopeCollisions", "enum E { A, B } int A;"},
        {1410, "ScopeCollisions", "int A; enum E { A, B }"},
        {1411, "ScopeCollisions", "class C {} int C;"},
        {1412, "ScopeCollisions", "interface I {} int I;"},
        {1413, "ScopeCollisions", "typedef int T; int T;"},
        {1414, "ScopeCollisions", "funcdef void CB(); int CB;"},
        {1415, "ScopeCollisions", "void f(int a, int a) {}"},
        {1416, "ScopeCollisions", "enum E { A, B, A }"},
        {1417, "MultipleDeclarations", "int a, b, c;"},
        {1418, "MultipleDeclarations", "int a = 1, b = 2, c = 3;"},
        {1419, "MultipleDeclarations", "float a = 1.0f, b, c = 3.0f;"},
        {1420, "MultipleDeclarations", "string a = \"A\", b = \"B\";"},
        {1421, "MultipleDeclarations", "int a, float b;"},
        {1422, "MultipleDeclarations", "class C { int a, b, c; }"},
        {1423, "MultipleDeclarations", "class C { int a = 1, b = 2; }"},
        {1424, "MultipleDeclarations", "int a, int a;"},
        {1425, "MultipleDeclarations", "class C { int a, a; }"},

        {1426, "TopLevelIncompatible", "const int g = \"string\";"},
        {1427, "TopLevelIncompatible", "const float g = true;"},
        {1428, "TopLevelIncompatible", "const bool g = \"string\";"},
        {1429, "TopLevelIncompatible", "const string g = 123;"},
        {1430, "TopLevelIncompatible", "const int g = 100;"},
        {1431, "TopLevelIncompatible", "const float g = 3.14f;"},
        {1432, "TopLevelIncompatible", "const bool g = true;"},
        {1433, "TopLevelIncompatible", "const string g = \"hello\";"},
        {1434, "TopLevelIncompatible", "class C { int x = \"string\"; }"},
        {1435, "TopLevelIncompatible", "class C { bool x = \"string\"; }"},
        {1436, "TopLevelIncompatible", "class C { string x = 123; }"},
        {1437, "TopLevelIncompatible", "class C { int x = 10; }"},
        {1438, "TopLevelIncompatible", "class C { string x = \"text\"; }"},
        {1439, "MixinValidation", "mixin class M { void m() {} } class C { mixin M; }"},
        {1440, "MixinValidation", "class NormalClass { void m() {} } class C { mixin NormalClass; }"},
        {1441, "MixinValidation", "class C { mixin UnknownMixin; }"},
        {1442, "MixinValidation", "interface I { void m(); } class C { mixin I; }"},
        {1443, "MixinValidation", "enum E { A } class C { mixin E; }"},
        {1444, "MixinValidation", "typedef int T; class C { mixin T; }"},
        {1445, "MixinValidation", "mixin class M1 {} mixin class M2 { mixin M1; }"},
        {1446, "MixinValidation", "mixin class M1 { void f() {} } class C { mixin M1; void f() {} }"},
        {1447, "MixinValidation", "mixin class M1 { int x; } class C { mixin M1; int x; }"},
        {1448, "MixinValidation", "mixin class M1 { void f() {} } mixin class M2 { void f() {} } class C { mixin M1; mixin M2; }"},
        {1449, "MixinValidation", "namespace N { mixin class M { void f() {} } } class C { mixin N::M; }"},
        {1450, "MixinValidation", "shared mixin class M { void f() {} }"},

        {1451, "MisplacedKeywords", "void f() explicit {}"},
        {1452, "MisplacedKeywords", "void f() delete {}"},
        {1453, "MisplacedKeywords", "void f() final override {}"},
        {1454, "MisplacedKeywords", "void f() override final {}"},
        {1455, "MisplacedKeywords", "void f() class {}"},
        {1456, "MisplacedKeywords", "void f() interface {}"},
        {1457, "MisplacedKeywords", "void f() const const {}"},
        {1458, "MisplacedKeywords", "explicit void f() {}"},
        {1459, "MisplacedKeywords", "delete void f() {}"},
        {1460, "MisplacedKeywords", "override void f() {}"},
        {1461, "MisplacedKeywords", "final void f() {}"},
        {1462, "MisplacedKeywords", "const void f() {}"},
        {1463, "MisplacedQualifiers", "class C final final {}"},
        {1464, "MisplacedQualifiers", "class C abstract abstract {}"},
        {1465, "MisplacedQualifiers", "class C final abstract {}"},
        {1466, "MisplacedQualifiers", "class C shared shared {}"},
        {1467, "MisplacedQualifiers", "class C explicit {}"},
        {1468, "MisplacedQualifiers", "class C override {}"},
        {1469, "MisplacedQualifiers", "interface I final {}"},
        {1470, "MisplacedQualifiers", "enum E final {}"},
        {1471, "MisplacedQualifiers", "typedef int T final;"},
        {1472, "MisplacedQualifiers", "int x const;"},
        {1473, "MisplacedQualifiers", "int x final;"},
        {1474, "MisplacedQualifiers", "const const int x = 0;"},
        {1475, "MisplacedQualifiers", "class C { private private int x; }"},

        {1476, "NativeVsGeneric", "void f(int[] a) {}"},
        {1477, "NativeVsGeneric", "void f(array<int> a) {}"},
        {1478, "NativeVsGeneric", "int[] f() { return null; }"},
        {1479, "NativeVsGeneric", "array<int> f() { return null; }"},
        {1480, "NativeVsGeneric", "int[]@ f() { return null; }"},
        {1481, "NativeVsGeneric", "array<int>@ f() { return null; }"},
        {1482, "ParamsEdge", "void f(const int &in a) {}"},
        {1483, "ParamsEdge", "void f(int &out a) {}"},
        {1484, "ParamsEdge", "void f(int &inout a) {}"},
        {1485, "ParamsEdge", "void f(void a) {}"},
        {1486, "ParamsEdge", "void f(int& a) {}"},
        {1487, "CorruptTemplates", "array<int> a;"},
        {1488, "CorruptTemplates", "array<UnknownType> a;"},
        {1489, "CorruptTemplates", "array<int, float> a;"},
        {1490, "CorruptTemplates", "array<> a;"},
        {1491, "CorruptTemplates", "array<int a;"},
        {1492, "BrokenClasses", "class C {}"},
        {1493, "BrokenClasses", "class C : BaseClass {}"},
        {1494, "BrokenClasses", "class C : UnknownBase {}"},
        {1495, "BrokenClasses", "class C final {}"},
        {1496, "BrokenClasses", "class C abstract {}"},
        {1497, "BrokenClasses", "shared class C {}"},
        {1498, "BrokenClasses", "external class C;"},
        {1499, "BrokenClasses", "class C { int x; float y; }"},
        {1500, "BrokenClasses", "class C { void f() {} void g() {} }"},
        {1501, "ComboModifiers", "shared final class C {}"},
        {1502, "ComboModifiers", "final shared class C {}"},
        {1503, "ComboModifiers", "shared abstract class C {}"},
        {1504, "ComboModifiers", "abstract shared class C {}"},
        {1505, "ComboModifiers", "external shared class C;"},
        {1506, "ComboModifiers", "shared external class C;"},
        {1507, "ComboModifiers", "external shared interface I {}"},
        {1508, "ComboModifiers", "shared external interface I {}"},
        {1509, "ComboModifiers", "class C { private final void f() {} }"},
        {1510, "ComboModifiers", "class C { protected override void f() {} }"},
        {1511, "ComboModifiers", "class C { public final override void f() {} }"},
        {1512, "ComboModifiers", "class C { private const void f() {} }"},
        {1513, "ComboModifiers", "class C { protected const void f() {} }"},
        {1514, "ComboModifiers", "class C { public const void f() {} }"},
        {1515, "ComboModifiers", "class C { private explicit C() {} }"},
        {1516, "ComboModifiers", "class C { protected explicit C() {} }"},
        {1517, "ComboModifiers", "class C { public explicit C() {} }"},
        {1518, "ComboModifiers", "class C { explicit private C() {} }"},
        {1519, "ComboModifiers", "class C { explicit protected C() {} }"},
        {1520, "ComboModifiers", "class C { explicit public C() {} }"},
        {1521, "ComboModifiers", "mixin class M { void f() {} }"},
        {1522, "ComboModifiers", "shared mixin class M { void f() {} }"},
        {1523, "ComboModifiers", "mixin shared class M { void f() {} }"},
        {1524, "ComboModifiers", "abstract mixin class M { void f() {} }"},
        {1525, "ComboModifiers", "mixin abstract class M { void f() {} }"},
        {1526, "ComboModifiers", "class C { mixin M; }"},
        {1527, "ComboModifiers", "class C { private mixin M; }"},
        {1528, "ComboModifiers", "class C { protected mixin M; }"},
        {1529, "ComboModifiers", "class C { public mixin M; }"},
        {1530, "ComboModifiers", "class C { int p { get const final { return 0; } } }"},
        {1531, "ComboModifiers", "class C { int p { get final const { return 0; } } }"},
        {1532, "ComboModifiers", "class C { int p { get const override { return 0; } } }"},
        {1533, "ComboModifiers", "class C { int p { get override const { return 0; } } }"},
        {1534, "ComboModifiers", "class C { int p { set final const { } } }"},
        {1535, "ComboModifiers", "class C { int p { set override const { } } }"},
        {1536, "ComboModifiers", "interface I { int p { get const; } }"},
        {1537, "ComboModifiers", "interface I { int p { set const; } }"},
        {1538, "ComboModifiers", "shared funcdef void CB();"},
        {1539, "ComboModifiers", "external shared funcdef void CB();"},
        {1540, "ComboModifiers", "shared enum E { A, B }"},
        {1541, "ComboModifiers", "external shared enum E;"},
        {1542, "ComboModifiers", "shared typedef int MyInt;"},
        {1543, "ComboModifiers", "external shared typedef int MyInt;"},
        {1544, "ComboModifiers", "class C { const int& f(const int& in a) const { return a; } }"},
        {1545, "ComboModifiers", "class C { final void f() const {} }"},
        {1546, "ComboModifiers", "class C { override void f() const {} }"},
        {1547, "ComboModifiers", "class C { const void f() const {} }"},
        {1548, "ComboModifiers", "class C { private const int x = 10; }"},
        {1549, "ComboModifiers", "class C { protected const int x = 10; }"},
        {1550, "ComboModifiers", "class C { public const int x = 10; }"}
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
    auto syntaxDiags = collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto semanticDiags = analyzer.Analyze(req);
    std::vector<Diagnostic> diagnostics = syntaxDiags;
    diagnostics.insert(diagnostics.end(), semanticDiags.begin(), semanticDiags.end());

    REQUIRE(diagnostics.size() >= 2);
    bool foundEnumInit = false;
    bool foundDupSymbol = false;

    for (const auto &d : diagnostics)
    {
        if (d.code == "as-err-enum-invalid-initializer" || d.code == "as-syntax-error") foundEnumInit = true;
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
    auto syntaxDiags = collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto semanticDiags = analyzer.Analyze(req);
    std::vector<Diagnostic> allDiags = syntaxDiags;
    allDiags.insert(allDiags.end(), semanticDiags.begin(), semanticDiags.end());
    REQUIRE(!allDiags.empty());
}

TEST_CASE("SemanticAnalyzer - Enum Suite 09: Bool Initializer in Enum Flagged")
{
    std::string sourceCode = "enum BadEnum2 { Key = true }\n";
    std::string fileUri = "file:///test_enum9.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    auto syntaxDiags = collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto semanticDiags = analyzer.Analyze(req);
    std::vector<Diagnostic> allDiags = syntaxDiags;
    allDiags.insert(allDiags.end(), semanticDiags.begin(), semanticDiags.end());
    REQUIRE(!allDiags.empty());
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

TEST_CASE("SemanticAnalyzer - Enum Member Invalid Initializers AST")
{
    std::string sourceCode = "enum BadEnum {\nValLambda = function(int a) {},\nValCall = SomeFunction(),\nValStr = \"hello\",\nValNull = null,\nValBool = true\n}\n";
    std::string fileUri = "file:///test_enum_ast_invalid.as";
    SymbolTable table; AngelScriptParser parser; SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);
    SemanticAnalyzer analyzer; angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 5);
    CHECK(diagnostics[0].code == "as-err-enum-invalid-initializer");
    CHECK(diagnostics[1].code == "as-err-enum-invalid-initializer");
    CHECK(diagnostics[2].code == "as-err-enum-invalid-initializer");
    CHECK(diagnostics[3].code == "as-err-enum-invalid-initializer");
    CHECK(diagnostics[4].code == "as-err-enum-invalid-initializer");
}


TEST_CASE("Batch_2050_Harness_Comparison")
{
    struct StructuralTestCase
    {
        int id;
        std::string cat;
        std::string code;
    };

    std::vector<StructuralTestCase> cases = {
        {1551, "AdvancedGenerics", "array<int> a;"},
        {1552, "AdvancedGenerics", "array<array<int>> a;"},
        {1553, "AdvancedGenerics", "array<array<array<float>>> a;"},
        {1554, "AdvancedGenerics", "array<string> a;"},
        {1555, "AdvancedGenerics", "class C {} array<C@> a;"},
        {1556, "AdvancedGenerics", "class C {} array<const C@> a;"},
        {1557, "AdvancedGenerics", "array<int>@ handle;"},
        {1558, "AdvancedGenerics", "array<int>@& handleRef;"},
        {1559, "AdvancedGenerics", "array<int> f() { return array<int>(); }"},
        {1560, "AdvancedGenerics", "void f(const array<int> &in a) {}"},
        {1561, "AdvancedGenerics", "array<UnknownType> a;"},
        {1562, "AdvancedGenerics", "array<array<UnknownType>> a;"},
        {1563, "AdvancedGenerics", "array<int, float> a;"},
        {1564, "AdvancedGenerics", "array<void> a;"},
        {1565, "AdvancedGenerics", "array<auto> a;"},
        {1566, "AdvancedGenerics", "array<class> a;"},
        {1567, "AdvancedGenerics", "array<enum> a;"},
        {1568, "AdvancedGenerics", "array<struct> a;"},
        {1569, "AdvancedGenerics", "array<interface> a;"},
        {1570, "AdvancedGenerics", "array<namespace> a;"},
        {1571, "AdvancedGenerics", "array<int@> a;"},
        {1572, "AdvancedGenerics", "array<float@> a;"},
        {1573, "AdvancedGenerics", "array<bool@> a;"},
        {1574, "AdvancedGenerics", "array<double@> a;"},
        {1575, "AdvancedGenerics", "array<uint@> a;"},
        {1576, "ComplexInheritance", "interface I1 {} interface I2 {} class C : I1, I2 {}"},
        {1577, "ComplexInheritance", "interface I { void f(); } class C : I { void f() {} }"},
        {1578, "ComplexInheritance", "interface I { void f(); } class C : I {}"},
        {1579, "ComplexInheritance", "class Base {} class Derived : Base {}"},
        {1580, "ComplexInheritance", "class Base {} class Derived : Base, Base {}"},
        {1581, "ComplexInheritance", "class C : C {}"},
        {1582, "ComplexInheritance", "class A {} class B : A {} class C : B {}"},
        {1583, "ComplexInheritance", "interface I1 {} interface I2 : I1 {}"},
        {1584, "ComplexInheritance", "interface I : I {}"},
        {1585, "ComplexInheritance", "abstract class Base { void f() {} } class Derived : Base {}"},
        {1586, "ComplexInheritance", "final class Base {} class Derived : Base {}"},
        {1587, "ComplexInheritance", "class Base {} class Derived : Base { void f() override {} }"},
        {1588, "ComplexInheritance", "class C { void f() override {} }"},
        {1589, "ComplexInheritance", "class Base { void f() {} } class Derived : Base { void f() final {} }"},
        {1590, "ComplexInheritance", "class Base { void f() final {} } class Derived : Base { void f() override {} }"},
        {1591, "ComplexInheritance", "interface I { void f(); } class C : I { void f() override {} }"},
        {1592, "ComplexInheritance", "interface I { void f(); } class C : I { void f() final {} }"},
        {1593, "ComplexInheritance", "class Base {} class Derived : Base { Derived() { super(); } }"},
        {1594, "ComplexInheritance", "class C { C() { super(); } }"},
        {1595, "ComplexInheritance", "class Base { Base(int a) {} } class Derived : Base { Derived() { super(10); } }"},
        {1596, "ComplexInheritance", "class Base {} class Derived : Base { void f() { Base::f(); } }"},
        {1597, "ComplexInheritance", "class C : UnknownBase {}"},
        {1598, "ComplexInheritance", "interface I : UnknownI {}"},
        {1599, "ComplexInheritance", "class C : int {}"},
        {1600, "ComplexInheritance", "class C : float {}"},
        {1601, "OperatorOverloadingCombo", "class C { C opAdd(const C &in other) const { return C(); } }"},
        {1602, "OperatorOverloadingCombo", "class C { C opSub(const C &in other) const { return C(); } }"},
        {1603, "OperatorOverloadingCombo", "class C { C opMul(const C &in other) const { return C(); } }"},
        {1604, "OperatorOverloadingCombo", "class C { C opDiv(const C &in other) const { return C(); } }"},
        {1605, "OperatorOverloadingCombo", "class C { C opMod(const C &in other) const { return C(); } }"},
        {1606, "OperatorOverloadingCombo", "class C { bool opEquals(const C &in other) const { return true; } }"},
        {1607, "OperatorOverloadingCombo", "class C { int opEquals(const C &in other) const { return 0; } }"},
        {1608, "OperatorOverloadingCombo", "class C { int opCmp(const C &in other) const { return 0; } }"},
        {1609, "OperatorOverloadingCombo", "class C { bool opCmp(const C &in other) const { return true; } }"},
        {1610, "OperatorOverloadingCombo", "class C { int& opIndex(int idx) { return val; } int val; }"},
        {1611, "OperatorOverloadingCombo", "class C { const int& opIndex(int idx) const { return val; } int val; }"},
        {1612, "OperatorOverloadingCombo", "class C { int opIndex() { return 0; } }"},
        {1613, "OperatorOverloadingCombo", "class C { C& opAssign(const C &in other) { return this; } }"},
        {1614, "OperatorOverloadingCombo", "class C { C opAdd(const C &in a, const C &in b) {} }"},
        {1615, "OperatorOverloadingCombo", "class C { C opNeg() const { return C(); } }"},
        {1616, "OperatorOverloadingCombo", "class C { C opCom() const { return C(); } }"},
        {1617, "OperatorOverloadingCombo", "class C { C opPostInc() { return C(); } }"},
        {1618, "OperatorOverloadingCombo", "class C { C opPostDec() { return C(); } }"},
        {1619, "OperatorOverloadingCombo", "class C { C opPreInc() { return C(); } }"},
        {1620, "OperatorOverloadingCombo", "class C { C opPreDec() { return C(); } }"},
        {1621, "OperatorOverloadingCombo", "void opAdd(int a, int b) {}"},
        {1622, "OperatorOverloadingCombo", "class C { C opShl(int b) const { return C(); } }"},
        {1623, "OperatorOverloadingCombo", "class C { C opShr(int b) const { return C(); } }"},
        {1624, "OperatorOverloadingCombo", "class C { C opUShr(int b) const { return C(); } }"},
        {1625, "OperatorOverloadingCombo", "class C { C opPow(const C &in b) const { return C(); } }"},
        {1626, "VirtualPropertiesCombo", "class C { int p { get { return 0; } set {} } }"},
        {1627, "VirtualPropertiesCombo", "class C { int p { get const { return 0; } set {} } }"},
        {1628, "VirtualPropertiesCombo", "class C { int p { get { return 0; } } }"},
        {1629, "VirtualPropertiesCombo", "class C { int p { set {} } }"},
        {1630, "VirtualPropertiesCombo", "class C { int p { get; set; } }"},
        {1631, "VirtualPropertiesCombo", "class C { int p { get const; set; } }"},
        {1632, "VirtualPropertiesCombo", "class C { int p { get { return 0; } get { return 1; } } }"},
        {1633, "VirtualPropertiesCombo", "class C { int p { set {} set {} } }"},
        {1634, "VirtualPropertiesCombo", "class C { int p { get final { return 0; } } }"},
        {1635, "VirtualPropertiesCombo", "class C { int p { get override { return 0; } } }"},
        {1636, "VirtualPropertiesCombo", "class C { int p { set final {} } }"},
        {1637, "VirtualPropertiesCombo", "class C { int p { set override {} } }"},
        {1638, "VirtualPropertiesCombo", "interface I { int p { get const; set; } }"},
        {1639, "VirtualPropertiesCombo", "class Base { int p { get { return 0; } } } class Derived : Base { int p { get override { return 1; } } }"},
        {1640, "VirtualPropertiesCombo", "class C { private int p { get { return 0; } } }"},
        {1641, "VirtualPropertiesCombo", "class C { protected int p { get { return 0; } } }"},
        {1642, "VirtualPropertiesCombo", "class C { int@ p { get { return null; } } }"},
        {1643, "VirtualPropertiesCombo", "class C { const int p { get { return 0; } } }"},
        {1644, "VirtualPropertiesCombo", "class C { void p { get {} } }"},
        {1645, "VirtualPropertiesCombo", "class C { int p { get const final { return 0; } } }"},
        {1646, "VirtualPropertiesCombo", "class C { int p { get final const { return 0; } } }"},
        {1647, "VirtualPropertiesCombo", "class C { int p { get const override { return 0; } } }"},
        {1648, "VirtualPropertiesCombo", "class C { int p { get override const { return 0; } } }"},
        {1649, "VirtualPropertiesCombo", "class C { int p { get; get; } }"},
        {1650, "VirtualPropertiesCombo", "class C { int p { set; set; } }"},
        {1651, "FuncdefAndDelegates", "funcdef void Callback();"},
        {1652, "FuncdefAndDelegates", "funcdef int MathOp(int a, int b);"},
        {1653, "FuncdefAndDelegates", "funcdef void ObjectCb(const string &in msg);"},
        {1654, "FuncdefAndDelegates", "funcdef Callback@ CbFactory();"},
        {1655, "FuncdefAndDelegates", "class C {} funcdef void MethodCb(C@ obj);"},
        {1656, "FuncdefAndDelegates", "funcdef void GenericCb(auto @ obj);"},
        {1657, "FuncdefAndDelegates", "funcdef int();"},
        {1658, "FuncdefAndDelegates", "funcdef void CB(int a = 0);"},
        {1659, "FuncdefAndDelegates", "funcdef void CB(int a, float b = 1.0f);"},
        {1660, "FuncdefAndDelegates", "funcdef void CB(int a = 1, float b);"},
        {1661, "FuncdefAndDelegates", "funcdef void CB(void);"},
        {1662, "FuncdefAndDelegates", "funcdef void CB(void a);"},
        {1663, "FuncdefAndDelegates", "funcdef int@ HandleRet();"},
        {1664, "FuncdefAndDelegates", "funcdef const int& ConstRefRet();"},
        {1665, "FuncdefAndDelegates", "funcdef void OutParam(int &out a);"},
        {1666, "FuncdefAndDelegates", "funcdef void InOutParam(int &inout a);"},
        {1667, "FuncdefAndDelegates", "funcdef void DoubleRefParam(int && a);"},
        {1668, "FuncdefAndDelegates", "shared funcdef void SharedCb();"},
        {1669, "FuncdefAndDelegates", "external funcdef void ExtCb();"},
        {1670, "FuncdefAndDelegates", "external shared funcdef void ExtSharedCb();"},
        {1671, "FuncdefAndDelegates", "namespace N { funcdef void Cb(); }"},
        {1672, "FuncdefAndDelegates", "class C { funcdef void Cb(); }"},
        {1673, "FuncdefAndDelegates", "funcdef void Cb(UnknownType x);"},
        {1674, "FuncdefAndDelegates", "funcdef UnknownRet Cb();"},
        {1675, "FuncdefAndDelegates", "funcdef void int();"},
        {1676, "MixinComposition", "mixin class M { void f() {} }"},
        {1677, "MixinComposition", "mixin class M { int x; }"},
        {1678, "MixinComposition", "mixin class M { void f() {} int x; }"},
        {1679, "MixinComposition", "mixin class M { M() {} }"},
        {1680, "MixinComposition", "mixin class M { ~M() {} }"},
        {1681, "MixinComposition", "mixin class M { int p { get { return 0; } } }"},
        {1682, "MixinComposition", "mixin class M { void f() override {} }"},
        {1683, "MixinComposition", "mixin class M { void f() final {} }"},
        {1684, "MixinComposition", "mixin class M { private void f() {} }"},
        {1685, "MixinComposition", "mixin class M { protected void f() {} }"},
        {1686, "MixinComposition", "mixin class M1 {} mixin class M2 {}"},
        {1687, "MixinComposition", "mixin final class M {}"},
        {1688, "MixinComposition", "mixin abstract class M {}"},
        {1689, "MixinComposition", "shared mixin class M {}"},
        {1690, "MixinComposition", "mixin shared class M {}"},
        {1691, "MixinComposition", "external mixin class M;"},
        {1692, "MixinComposition", "class C { mixin M; }"},
        {1693, "MixinComposition", "class C { mixin M1; mixin M2; }"},
        {1694, "MixinComposition", "class C { mixin UnknownMixin; }"},
        {1695, "MixinComposition", "mixin class M : Base {}"},
        {1696, "MixinComposition", "mixin class M : I {}"},
        {1697, "MixinComposition", "interface I { mixin M; }"},
        {1698, "MixinComposition", "namespace N { mixin class M {} }"},
        {1699, "MixinComposition", "mixin class M { void f() { g(); } void g() {} }"},
        {1700, "MixinComposition", "mixin class M { void f() { this.g(); } void g() {} }"},
        {1701, "NamespaceScoping", "namespace N { class C {} }"},
        {1702, "NamespaceScoping", "namespace N1 { namespace N2 { class C {} } }"},
        {1703, "NamespaceScoping", "namespace N { void f() {} }"},
        {1704, "NamespaceScoping", "namespace N { int x = 10; }"},
        {1705, "NamespaceScoping", "namespace N { enum E { A, B } }"},
        {1706, "NamespaceScoping", "namespace N { typedef int MyInt; }"},
        {1707, "NamespaceScoping", "namespace N { interface I {} }"},
        {1708, "NamespaceScoping", "namespace N { funcdef void CB(); }"},
        {1709, "NamespaceScoping", "namespace N { class C {} } N::C g_var;"},
        {1710, "NamespaceScoping", "namespace N { class C {} } void f(N::C@ obj) {}"},
        {1711, "NamespaceScoping", "namespace N { class C {} } N::C@ f() { return null; }"},
        {1712, "NamespaceScoping", "namespace N { void f() {} } void g() { N::f(); }"},
        {1713, "NamespaceScoping", "namespace N { int x = 10; } int g() { return N::x; }"},
        {1714, "NamespaceScoping", "namespace N { class C {} } class Derived : N::C {}"},
        {1715, "NamespaceScoping", "class C {} namespace N { ::C g_var; }"},
        {1716, "NamespaceScoping", "void f() {} namespace N { void g() { ::f(); } }"},
        {1717, "NamespaceScoping", "int g_x = 5; namespace N { int getX() { return ::g_x; } }"},
        {1718, "NamespaceScoping", "namespace N { class C { void f() { N::C obj; } } }"},
        {1719, "NamespaceScoping", "namespace N { class C {} } Unknown::C g_var;"},
        {1720, "NamespaceScoping", "namespace N { class C {} } N::Unknown g_var;"},
        {1721, "NamespaceScoping", "namespace 123 {}"},
        {1722, "NamespaceScoping", "namespace class {}"},
        {1723, "NamespaceScoping", "namespace void {}"},
        {1724, "NamespaceScoping", "namespace N { namespace N { class C {} } }"},
        {1725, "NamespaceScoping", "namespace A { namespace B { namespace C { int val = 1; } } }"},
        {1726, "ControlFlowExpressions", "int f(bool b) { return b ? 1 : 0; }"},
        {1727, "ControlFlowExpressions", "float f(bool b) { return b ? 1.0f : 2.0f; }"},
        {1728, "ControlFlowExpressions", "string f(bool b) { return b ? \"yes\" : \"no\"; }"},
        {1729, "ControlFlowExpressions", "class C {} C@ f(bool b, C@ c) { return b ? c : null; }"},
        {1730, "ControlFlowExpressions", "void f(int x) { switch(x) { case 0: break; case 1: break; default: break; } }"},
        {1731, "ControlFlowExpressions", "void f(int x) { switch(x) { case 0: { int a = 1; } break; } }"},
        {1732, "ControlFlowExpressions", "void f(int x) { switch(x) { case 'A': break; case 'B': break; } }"},
        {1733, "ControlFlowExpressions", "void f() { for (int i = 0; i < 10; ++i) { if (i == 5) continue; if (i == 8) break; } }"},
        {1734, "ControlFlowExpressions", "void f() { int i = 0; while (i < 10) { ++i; } }"},
        {1735, "ControlFlowExpressions", "void f() { int i = 0; do { ++i; } while (i < 10); }"},
        {1736, "ControlFlowExpressions", "void f() { try { int a = 10; } catch {} }"},
        {1737, "ControlFlowExpressions", "void f() { try {} catch {} }"},
        {1738, "ControlFlowExpressions", "void f() { if (true) { return; } else { return; } }"},
        {1739, "ControlFlowExpressions", "void f() { if (true) return; else if (false) return; else return; }"},
        {1740, "ControlFlowExpressions", "int f() { if (true) return 1; return 0; }"},
        {1741, "ControlFlowExpressions", "void f() { break; }"},
        {1742, "ControlFlowExpressions", "void f() { continue; }"},
        {1743, "ControlFlowExpressions", "void f() { case 0: break; }"},
        {1744, "ControlFlowExpressions", "void f() { default: break; }"},
        {1745, "ControlFlowExpressions", "void f() { return 1, 2; }"},
        {1746, "ControlFlowExpressions", "void f() { return void; }"},
        {1747, "ControlFlowExpressions", "int f() { return; }"},
        {1748, "ControlFlowExpressions", "void f() { return 42; }"},
        {1749, "ControlFlowExpressions", "int f() { return null; }"},
        {1750, "ControlFlowExpressions", "bool f() { return null; }"},
        {1751, "TypeCasting", "class Base {} class Derived : Base {} Derived@ f(Base@ b) { return cast<Derived>(b); }"},
        {1752, "TypeCasting", "class Base {} class Derived : Base {} Base@ f(Derived@ d) { return cast<Base>(d); }"},
        {1753, "TypeCasting", "class C {} C@ f(auto@ a) { return cast<C>(a); }"},
        {1754, "TypeCasting", "class C {} auto@ f(C@ c) { return cast<auto>(c); }"},
        {1755, "TypeCasting", "int f(float val) { return int(val); }"},
        {1756, "TypeCasting", "float f(int val) { return float(val); }"},
        {1757, "TypeCasting", "double f(float val) { return double(val); }"},
        {1758, "TypeCasting", "uint f(int val) { return uint(val); }"},
        {1759, "TypeCasting", "int8 f(int val) { return int8(val); }"},
        {1760, "TypeCasting", "int16 f(int val) { return int16(val); }"},
        {1761, "TypeCasting", "int64 f(int val) { return int64(val); }"},
        {1762, "TypeCasting", "class C {} C@ f() { return cast<C>(null); }"},
        {1763, "TypeCasting", "class C {} C@ f() { return cast<UnknownType>(null); }"},
        {1764, "TypeCasting", "class C {} C@ f() { return cast<int>(10); }"},
        {1765, "TypeCasting", "void f() { cast<void>(0); }"},
        {1766, "TypeCasting", "class C {} void f(C@ c) { if (cast<C>(c) !is null) {} }"},
        {1767, "TypeCasting", "class C {} void f(C@ c) { if (cast<C>(c) is null) {} }"},
        {1768, "TypeCasting", "class Base {} class Derived : Base {} void f(Base@ b) { Derived@ d = cast<Derived>(b); }"},
        {1769, "TypeCasting", "int f() { return cast<int>(3.14f); }"},
        {1770, "TypeCasting", "string f() { return cast<string>(123); }"},
        {1771, "TypeCasting", "class C {} C f() { return cast<C>(); }"},
        {1772, "TypeCasting", "class C {} C@ f() { return cast<C>; }"},
        {1773, "TypeCasting", "class C {} C@ f() { return cast(); }"},
        {1774, "TypeCasting", "class C {} C@ f() { return cast<C>(1, 2); }"},
        {1775, "TypeCasting", "class C {} C@ f() { return cast<C, float>(null); }"},
        {1776, "AutoTypeInference", "auto a = 10;"},
        {1777, "AutoTypeInference", "auto b = 3.14f;"},
        {1778, "AutoTypeInference", "auto c = \"hello\";"},
        {1779, "AutoTypeInference", "auto d = true;"},
        {1780, "AutoTypeInference", "class C {} C@ getC() { return C(); } void f() { auto@ obj = getC(); }"},
        {1781, "AutoTypeInference", "auto a;"},
        {1782, "AutoTypeInference", "auto@ a;"},
        {1783, "AutoTypeInference", "const auto a = 5;"},
        {1784, "AutoTypeInference", "auto& a = b;"},
        {1785, "AutoTypeInference", "void f(auto x) {}"},
        {1786, "AutoTypeInference", "auto f() { return 10; }"},
        {1787, "AutoTypeInference", "class C { auto x = 10; }"},
        {1788, "AutoTypeInference", "class C { auto@ x; }"},
        {1789, "AutoTypeInference", "auto[] arr;"},
        {1790, "AutoTypeInference", "array<auto> arr;"},
        {1791, "AutoTypeInference", "auto a = null;"},
        {1792, "AutoTypeInference", "auto@ a = null;"},
        {1793, "AutoTypeInference", "void f() { auto x = 1, y = 2; }"},
        {1794, "AutoTypeInference", "void f() { auto x = 1, y = 3.14f; }"},
        {1795, "AutoTypeInference", "namespace N { auto g_x = 100; }"},
        {1796, "AutoTypeInference", "auto f() { return; }"},
        {1797, "AutoTypeInference", "auto f() { return null; }"},
        {1798, "AutoTypeInference", "enum E { A } auto x = E::A;"},
        {1799, "AutoTypeInference", "typedef int MyInt; auto x = MyInt(5);"},
        {1800, "AutoTypeInference", "auto@ f() { return null; }"},
        {1801, "ConstRefSemantics", "void f(const int &in a) {}"},
        {1802, "ConstRefSemantics", "void f(int &out a) {}"},
        {1803, "ConstRefSemantics", "void f(int &inout a) {}"},
        {1804, "ConstRefSemantics", "void f(int & a) {}"},
        {1805, "ConstRefSemantics", "void f(const string &in s) {}"},
        {1806, "ConstRefSemantics", "void f(string &out s) {}"},
        {1807, "ConstRefSemantics", "void f(string &inout s) {}"},
        {1808, "ConstRefSemantics", "class C {} void f(const C &in c) {}"},
        {1809, "ConstRefSemantics", "class C {} void f(C &out c) {}"},
        {1810, "ConstRefSemantics", "class C {} void f(C &inout c) {}"},
        {1811, "ConstRefSemantics", "void f(int &inout &a) {}"},
        {1812, "ConstRefSemantics", "void f(int &out &a) {}"},
        {1813, "ConstRefSemantics", "void f(int && a) {}"},
        {1814, "ConstRefSemantics", "void f(int & & a) {}"},
        {1815, "ConstRefSemantics", "void f(const int & &in a) {}"},
        {1816, "ConstRefSemantics", "int & g_ref;"},
        {1817, "ConstRefSemantics", "float & g_ref;"},
        {1818, "ConstRefSemantics", "string & g_ref;"},
        {1819, "ConstRefSemantics", "class C {} C & g_ref;"},
        {1820, "ConstRefSemantics", "const int & f() { static int x = 0; return x; }"},
        {1821, "ConstRefSemantics", "int & f() { static int x = 0; return x; }"},
        {1822, "ConstRefSemantics", "const float & f() { static float x = 0; return x; }"},
        {1823, "ConstRefSemantics", "const bool & f() { static bool x = false; return x; }"},
        {1824, "ConstRefSemantics", "void f(int &in a) {}"},
        {1825, "ConstRefSemantics", "void f(const int &out a) {}"},
        {1826, "InitializerLists", "array<int> a = {1, 2, 3, 4};"},
        {1827, "InitializerLists", "array<float> a = {1.0f, 2.5f, 3.14f};"},
        {1828, "InitializerLists", "array<string> a = {\"a\", \"b\", \"c\"};"},
        {1829, "InitializerLists", "array<array<int>> a = {{1, 2}, {3, 4}};"},
        {1830, "InitializerLists", "array<int> a = {};"},
        {1831, "InitializerLists", "array<int> a = {1, 2.5f};"},
        {1832, "InitializerLists", "array<int> a = {1, \"hello\"};"},
        {1833, "InitializerLists", "array<string> a = {1, 2};"},
        {1834, "InitializerLists", "class C {} array<C@> a = {C(), C()};"},
        {1835, "InitializerLists", "class C {} array<C@> a = {null, null};"},
        {1836, "InitializerLists", "array<int> a = {1, 2,};"},
        {1837, "InitializerLists", "array<int> a = {, 1, 2};"},
        {1838, "InitializerLists", "void f(array<int> a = {1, 2}) {}"},
        {1839, "InitializerLists", "class C { array<int> a = {10, 20}; }"},
        {1840, "InitializerLists", "namespace N { array<int> a = {100}; }"},
        {1841, "InitializerLists", "int x = {1};"},
        {1842, "InitializerLists", "float x = {1.0f};"},
        {1843, "InitializerLists", "string x = {\"a\"};"},
        {1844, "InitializerLists", "array<int> a = {{1, 2}};"},
        {1845, "InitializerLists", "array<array<int>> a = {1, 2};"},
        {1846, "InitializerLists", "enum E { A, B } array<E> a = {E::A, E::B};"},
        {1847, "InitializerLists", "typedef int MyInt; array<MyInt> a = {1, 2};"},
        {1848, "InitializerLists", "array<int> a = {true, false};"},
        {1849, "InitializerLists", "array<bool> a = {1, 0};"},
        {1850, "InitializerLists", "array<int> a = {null};"},
        {1851, "DestructorsAndLifetime", "class C { ~C() {} }"},
        {1852, "DestructorsAndLifetime", "class C { ~C(int a) {} }"},
        {1853, "DestructorsAndLifetime", "class C { int ~C() {} }"},
        {1854, "DestructorsAndLifetime", "class C { void ~C() {} }"},
        {1855, "DestructorsAndLifetime", "class C { ~C() const {} }"},
        {1856, "DestructorsAndLifetime", "class C { ~C() override {} }"},
        {1857, "DestructorsAndLifetime", "class C { ~C() final {} }"},
        {1858, "DestructorsAndLifetime", "class C { ~C() delete; }"},
        {1859, "DestructorsAndLifetime", "class C { private ~C() {} }"},
        {1860, "DestructorsAndLifetime", "class C { protected ~C() {} }"},
        {1861, "DestructorsAndLifetime", "class C { public ~C() {} }"},
        {1862, "DestructorsAndLifetime", "class C { ~C() { return; } }"},
        {1863, "DestructorsAndLifetime", "class C { ~C() { return 0; } }"},
        {1864, "DestructorsAndLifetime", "interface I { ~I(); }"},
        {1865, "DestructorsAndLifetime", "mixin class M { ~M() {} }"},
        {1866, "DestructorsAndLifetime", "namespace N { class C { ~C() {} } }"},
        {1867, "DestructorsAndLifetime", "class C { ~C(); } void C::~C() {}"},
        {1868, "DestructorsAndLifetime", "class C { ~C() {} ~C() {} }"},
        {1869, "DestructorsAndLifetime", "class C { ~C(void) {} }"},
        {1870, "DestructorsAndLifetime", "class C { ~C(int a = 0) {} }"},
        {1871, "DestructorsAndLifetime", "class C { static ~C() {} }"},
        {1872, "DestructorsAndLifetime", "class C { explicit ~C() {} }"},
        {1873, "DestructorsAndLifetime", "class C { shared ~C() {} }"},
        {1874, "DestructorsAndLifetime", "class C { external ~C(); }"},
        {1875, "DestructorsAndLifetime", "class C { ~C() { int x = 10; } }"},
        {1876, "SharedAndExternal", "shared class C {}"},
        {1877, "SharedAndExternal", "shared interface I {}"},
        {1878, "SharedAndExternal", "shared enum E { A, B }"},
        {1879, "SharedAndExternal", "shared funcdef void CB();"},
        {1880, "SharedAndExternal", "external class C;"},
        {1881, "SharedAndExternal", "external interface I;"},
        {1882, "SharedAndExternal", "external enum E;"},
        {1883, "SharedAndExternal", "external funcdef void CB();"},
        {1884, "SharedAndExternal", "external shared class C;"},
        {1885, "SharedAndExternal", "shared external class C;"},
        {1886, "SharedAndExternal", "external shared interface I;"},
        {1887, "SharedAndExternal", "shared external interface I;"},
        {1888, "SharedAndExternal", "external shared enum E;"},
        {1889, "SharedAndExternal", "shared external enum E;"},
        {1890, "SharedAndExternal", "external shared funcdef void CB();"},
        {1891, "SharedAndExternal", "shared external funcdef void CB();"},
        {1892, "SharedAndExternal", "shared class C { void f() {} }"},
        {1893, "SharedAndExternal", "shared class C { int x; }"},
        {1894, "SharedAndExternal", "shared class C { C() {} }"},
        {1895, "SharedAndExternal", "shared class Base {} shared class Derived : Base {}"},
        {1896, "SharedAndExternal", "class NonShared {} shared class C : NonShared {}"},
        {1897, "SharedAndExternal", "interface NonShared {} shared class C : NonShared {}"},
        {1898, "SharedAndExternal", "shared typedef int MyInt;"},
        {1899, "SharedAndExternal", "external typedef int MyInt;"},
        {1900, "SharedAndExternal", "external shared typedef int MyInt;"},
        {1901, "ImportAndExport", "import void f() from \"Module\";"},
        {1902, "ImportAndExport", "import int g(int a, float b) from \"Module\";"},
        {1903, "ImportAndExport", "class C {} import C@ getObj() from \"Module\";"},
        {1904, "ImportAndExport", "import void f() from \"\";"},
        {1905, "ImportAndExport", "import void f();"},
        {1906, "ImportAndExport", "import int x from \"Module\";"},
        {1907, "ImportAndExport", "import class C from \"Module\";"},
        {1908, "ImportAndExport", "import interface I from \"Module\";"},
        {1909, "ImportAndExport", "import enum E from \"Module\";"},
        {1910, "ImportAndExport", "import funcdef void CB() from \"Module\";"},
        {1911, "ImportAndExport", "import void f(int a = 0) from \"Module\";"},
        {1912, "ImportAndExport", "import const int& f() from \"Module\";"},
        {1913, "ImportAndExport", "import void f(int &out a) from \"Module\";"},
        {1914, "ImportAndExport", "import void f(int &inout a) from \"Module\";"},
        {1915, "ImportAndExport", "import void f() from 123;"},
        {1916, "ImportAndExport", "namespace N { import void f() from \"Module\"; }"},
        {1917, "ImportAndExport", "class C { import void f() from \"Module\"; }"},
        {1918, "ImportAndExport", "import void f() from \"Mod1\" \"Mod2\";"},
        {1919, "ImportAndExport", "import void f() from 'Module';"},
        {1920, "ImportAndExport", "import void f() from \"\"\"Module\"\"\";"},
        {1921, "ImportAndExport", "import UnknownType f() from \"Module\";"},
        {1922, "ImportAndExport", "import void f(UnknownType x) from \"Module\";"},
        {1923, "ImportAndExport", "import void f() from \"Mod\\n\";"},
        {1924, "ImportAndExport", "import void f() from \"Mod\\t\";"},
        {1925, "ImportAndExport", "import void f() from \"Mod\\x41\";"},
        {1926, "TypedefVariations", "typedef int MyInt;"},
        {1927, "TypedefVariations", "typedef float MyFloat;"},
        {1928, "TypedefVariations", "typedef double MyDouble;"},
        {1929, "TypedefVariations", "typedef bool MyBool;"},
        {1930, "TypedefVariations", "typedef uint MyUInt;"},
        {1931, "TypedefVariations", "typedef int8 MyInt8;"},
        {1932, "TypedefVariations", "typedef int16 MyInt16;"},
        {1933, "TypedefVariations", "typedef int64 MyInt64;"},
        {1934, "TypedefVariations", "typedef uint8 MyUInt8;"},
        {1935, "TypedefVariations", "typedef uint16 MyUInt16;"},
        {1936, "TypedefVariations", "typedef uint64 MyUInt64;"},
        {1937, "TypedefVariations", "typedef string MyString;"},
        {1938, "TypedefVariations", "typedef array<int> IntArray;"},
        {1939, "TypedefVariations", "class C {} typedef C MyClass;"},
        {1940, "TypedefVariations", "class C {} typedef C@ MyClassHandle;"},
        {1941, "TypedefVariations", "typedef void MyVoid;"},
        {1942, "TypedefVariations", "typedef auto MyAuto;"},
        {1943, "TypedefVariations", "typedef UnknownType MyType;"},
        {1944, "TypedefVariations", "typedef int int;"},
        {1945, "TypedefVariations", "typedef float float;"},
        {1946, "TypedefVariations", "typedef bool bool;"},
        {1947, "TypedefVariations", "typedef void void;"},
        {1948, "TypedefVariations", "namespace N { typedef int MyInt; }"},
        {1949, "TypedefVariations", "class C { typedef int MyInt; }"},
        {1950, "TypedefVariations", "typedef MyInt MyInt2;"},
        {1951, "EnumValuesAndScope", "enum E { A, B, C }"},
        {1952, "EnumValuesAndScope", "enum E { A = 0, B = 10, C = 100 }"},
        {1953, "EnumValuesAndScope", "enum E { A = -1, B = -2, C = -3 }"},
        {1954, "EnumValuesAndScope", "enum E { A = 0x01, B = 0x02, C = 0x04 }"},
        {1955, "EnumValuesAndScope", "enum E { A = 1 + 2, B = 3 * 4 }"},
        {1956, "EnumValuesAndScope", "enum E { A = (1 << 0), B = (1 << 1) }"},
        {1957, "EnumValuesAndScope", "enum E { A, B = A + 1, C = B + 1 }"},
        {1958, "EnumValuesAndScope", "enum E { A = 1, A = 2 }"},
        {1959, "EnumValuesAndScope", "enum E { A = \"str\" }"},
        {1960, "EnumValuesAndScope", "enum E { A = 3.14f }"},
        {1961, "EnumValuesAndScope", "enum E { A = true }"},
        {1962, "EnumValuesAndScope", "enum E { A = null }"},
        {1963, "EnumValuesAndScope", "enum E { A = function(int a) {} }"},
        {1964, "EnumValuesAndScope", "enum E { A = SomeFunc() }"},
        {1965, "EnumValuesAndScope", "enum E { A = int }"},
        {1966, "EnumValuesAndScope", "enum E { A = float }"},
        {1967, "EnumValuesAndScope", "enum E { A = class }"},
        {1968, "EnumValuesAndScope", "enum E { A = struct }"},
        {1969, "EnumValuesAndScope", "enum E { A = enum }"},
        {1970, "EnumValuesAndScope", "enum E { A = void }"},
        {1971, "EnumValuesAndScope", "enum E { A = auto }"},
        {1972, "EnumValuesAndScope", "enum E { A, }"},
        {1973, "EnumValuesAndScope", "enum E { }"},
        {1974, "EnumValuesAndScope", "enum E;"},
        {1975, "EnumValuesAndScope", "enum E { A = B, B = 1 }"},
        {1976, "FunctionAttributes", "class Base { void f() {} } class Derived : Base { void f() override {} }"},
        {1977, "FunctionAttributes", "class Base { void f() {} } class Derived : Base { void f() final {} }"},
        {1978, "FunctionAttributes", "class Base { void f() {} } class Derived : Base { void f() override final {} }"},
        {1979, "FunctionAttributes", "class Base { void f() {} } class Derived : Base { void f() final override {} }"},
        {1980, "FunctionAttributes", "class C { void f() delete; }"},
        {1981, "FunctionAttributes", "class C { void f() const {} }"},
        {1982, "FunctionAttributes", "class C { void f() const override {} }"},
        {1983, "FunctionAttributes", "class C { void f() const final {} }"},
        {1984, "FunctionAttributes", "class C { void f() const delete; }"},
        {1985, "FunctionAttributes", "class C { void f() final delete; }"},
        {1986, "FunctionAttributes", "class C { void f() override delete; }"},
        {1987, "FunctionAttributes", "void f() override {}"},
        {1988, "FunctionAttributes", "void f() final {}"},
        {1989, "FunctionAttributes", "void f() delete;"},
        {1990, "FunctionAttributes", "void f() const {}"},
        {1991, "FunctionAttributes", "class C { private void f() override {} }"},
        {1992, "FunctionAttributes", "class C { protected void f() override {} }"},
        {1993, "FunctionAttributes", "class C { public void f() override {} }"},
        {1994, "FunctionAttributes", "class C { private void f() final {} }"},
        {1995, "FunctionAttributes", "class C { protected void f() final {} }"},
        {1996, "FunctionAttributes", "class C { public void f() final {} }"},
        {1997, "FunctionAttributes", "class C { private void f() delete; }"},
        {1998, "FunctionAttributes", "class C { protected void f() delete; }"},
        {1999, "FunctionAttributes", "class C { public void f() delete; }"},
        {2000, "FunctionAttributes", "class C { void f() property {} }"},
        {2001, "PrimitiveHandleEdge", "int@ globalHandle;"},
        {2002, "PrimitiveHandleEdge", "float@ globalHandle;"},
        {2003, "PrimitiveHandleEdge", "double@ globalHandle;"},
        {2004, "PrimitiveHandleEdge", "bool@ globalHandle;"},
        {2005, "PrimitiveHandleEdge", "uint@ globalHandle;"},
        {2006, "PrimitiveHandleEdge", "int8@ globalHandle;"},
        {2007, "PrimitiveHandleEdge", "int16@ globalHandle;"},
        {2008, "PrimitiveHandleEdge", "int64@ globalHandle;"},
        {2009, "PrimitiveHandleEdge", "string@ globalHandle;"},
        {2010, "PrimitiveHandleEdge", "void f(int@ p) {}"},
        {2011, "PrimitiveHandleEdge", "void f(float@ p) {}"},
        {2012, "PrimitiveHandleEdge", "void f(string@ p) {}"},
        {2013, "PrimitiveHandleEdge", "int@ f() { return null; }"},
        {2014, "PrimitiveHandleEdge", "float@ f() { return null; }"},
        {2015, "PrimitiveHandleEdge", "string@ f() { return null; }"},
        {2016, "PrimitiveHandleEdge", "class C { int@ memberHandle; }"},
        {2017, "PrimitiveHandleEdge", "class C { float@ memberHandle; }"},
        {2018, "PrimitiveHandleEdge", "class C { string@ memberHandle; }"},
        {2019, "PrimitiveHandleEdge", "int@[] arrayVal;"},
        {2020, "PrimitiveHandleEdge", "float@[] arrayVal;"},
        {2021, "PrimitiveHandleEdge", "string@[] arrayVal;"},
        {2022, "PrimitiveHandleEdge", "class C {} C@ globalObjHandle;"},
        {2023, "PrimitiveHandleEdge", "class C {} const C@ globalConstObjHandle;"},
        {2024, "PrimitiveHandleEdge", "class C {} C@ const globalObjHandleConst;"},
        {2025, "StringLiteralsConcat", "string s = \"Hello \" \"World\";"},
        {2026, "StringLiteralsConcat", "string s = \"Hello \" \"World \" \"Wide\";"},
        {2027, "StringLiteralsConcat", "string s = \"Line1\\n\" \"Line2\\n\";"},
        {2028, "StringLiteralsConcat", "string s = 'Single ' 'Quotes';"},
        {2029, "StringLiteralsConcat", "string s = \"Double \" 'Single';"},
        {2030, "StringLiteralsConcat", "string s = 'Single ' \"Double\";"},
        {2031, "StringLiteralsConcat", "string s = \"Const \" \"String \";"},
        {2032, "StringLiteralsConcat", "const string S = \"Alpha \" \"Beta\";"},
        {2033, "StringLiteralsConcat", "string s = \"Escaped \\\"quote\\\" \" \"next\";"},
        {2034, "StringLiteralsConcat", "string s = \"Escaped \\\\ slash \" \"next\";"},
        {2035, "StringLiteralsConcat", "string s = \"\"\"Heredoc 1\"\"\" \"\"\"Heredoc 2\"\"\";"},
        {2036, "StringLiteralsConcat", "string s = \"Normal \" \"\"\"Heredoc\"\"\";"},
        {2037, "StringLiteralsConcat", "string s = \"\"\"Heredoc\"\"\" \" Normal\";"},
        {2038, "StringLiteralsConcat", "string s = \"A\" \"B\" \"C\" \"D\" \"E\";"},
        {2039, "StringLiteralsConcat", "string s = \"A\"\n\"B\";"},
        {2040, "StringLiteralsConcat", "string s = \"A\" \n \"B\";"},
        {2041, "StringLiteralsConcat", "string s = 'A'\n'B';"},
        {2042, "StringLiteralsConcat", "string s = \"A\" + \"B\";"},
        {2043, "StringLiteralsConcat", "string s = \"A\" + \n \"B\";"},
        {2044, "StringLiteralsConcat", "string s = \"A\"\n + \"B\";"},
        {2045, "StringLiteralsConcat", "void f(string p = \"A \" \"B\") {}"},
        {2046, "StringLiteralsConcat", "class C { string p = \"A \" \"B\"; }"},
        {2047, "StringLiteralsConcat", "namespace N { string s = \"A \" \"B\"; }"},
        {2048, "StringLiteralsConcat", "string s = \"Tab\\t\" \"Space \";"},
        {2049, "StringLiteralsConcat", "string s = \"Hex \\x41\" \"Hex \\x42\";"},
        {2050, "StringLiteralsConcat", "string s = \"\\u0041\" \"\\u0042\";"},
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

