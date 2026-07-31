#include <doctest/doctest.h>

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

