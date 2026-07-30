#include <doctest/doctest.h>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("SemanticAnalyzer - Mixin Final Error Test")
{
    std::string sourceCode = "mixin final class InvalidMixin {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    auto syms = table.FindSymbols("InvalidMixin");
    REQUIRE(syms.size() == 1);
    CHECK(syms[0].classSignature.modifiers.isMixin == true);
    CHECK(syms[0].classSignature.modifiers.isFinal == true);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-mixin-final");
    CHECK(diagnostics[0].severity == DiagnosticSeverity::Error);
}

TEST_CASE("SemanticAnalyzer - Inherit From Final Class Error Test")
{
    std::string sourceCode = R"(
        final class BaseFinal {}
        class Derived : BaseFinal {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    auto baseSyms = table.FindSymbols("BaseFinal");
    REQUIRE(baseSyms.size() == 1);
    CHECK(baseSyms[0].classSignature.modifiers.isFinal == true);

    auto devSyms = table.FindSymbols("Derived");
    REQUIRE(devSyms.size() == 1);
    REQUIRE(devSyms[0].classSignature.bases.size() == 1);
    CHECK(devSyms[0].classSignature.bases[0] == "BaseFinal");

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-inherit-final");
}

TEST_CASE("SemanticAnalyzer - Duplicate Class Symbol Error Test")
{
    std::string sourceCode = R"(
        class MyClass {}
        class MyClass {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-duplicate-symbol");
}

TEST_CASE("SemanticAnalyzer - Template Class Validation Test")
{
    std::string sourceCode = "class array<T> {}\n";

    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    SUBCASE("Template class in .as file triggers error")
    {
        std::string fileUri = "file:///test.as";
        SymbolTable table;
        collector.CollectSymbols(fileUri, sourceCode, parser, table);

        auto syms = table.FindSymbols("array");
        REQUIRE(syms.size() == 1);
        CHECK(syms[0].classSignature.isTemplate == true);

        SemanticAnalyzer analyzer;
        SemanticAnalysisRequest req{table, fileUri, "as.predefined"};
        auto diagnostics = analyzer.Analyze(req);

        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "as-err-template-class-not-supported");
    }

    SUBCASE("Template class in .as.predefined file is allowed")
    {
        std::string fileUri = "file:///as.predefined";
        SymbolTable table;
        collector.CollectSymbols(fileUri, sourceCode, parser, table);

        auto syms = table.FindSymbols("array");
        REQUIRE(syms.size() == 1);
        CHECK(syms[0].classSignature.isTemplate == true);

        SemanticAnalyzer analyzer;
        SemanticAnalysisRequest req{table, fileUri, "as.predefined"};
        auto diagnostics = analyzer.Analyze(req);

        CHECK(diagnostics.empty());
    }
}

TEST_CASE("SemanticAnalyzer - I18n Locale Messages Test")
{
    std::string sourceCode = "void test();\n";
    std::string fileUri = "file:///test.as";

    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    SymbolTable table;
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;

    SUBCASE("English Locale")
    {
        angel_lsp::i18n::I18n i18nEn("en");
        SemanticAnalysisRequest req{table, fileUri, "as.predefined", &i18nEn};
        auto diagnostics = analyzer.Analyze(req);

        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].message == "Function 'test' must have a body '{}'.");
    }

    SUBCASE("Spanish Locale")
    {
        angel_lsp::i18n::I18n i18nEs("es");
        SemanticAnalysisRequest req{table, fileUri, "as.predefined", &i18nEs};
        auto diagnostics = analyzer.Analyze(req);

        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].message == "La función 'test' debe tener un cuerpo '{}'.");
    }
}

TEST_CASE("SemanticAnalyzer - Unresolved Variable Type Test")
{
    std::string sourceCode = "SomeUnknownType x;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-unresolved-type");
}

TEST_CASE("SemanticAnalyzer - Handle On Primitive Type Error Test")
{
    std::string sourceCode = "int@ x;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-handle-on-primitive");
}

TEST_CASE("SemanticAnalyzer - Void Variable Error Test")
{
    std::string sourceCode = "void x;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-void-variable");
}

TEST_CASE("SemanticAnalyzer - Base Class Not Found Test")
{
    std::string sourceCode = "class Derived : NonExistentBase {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-base-not-found");
}

TEST_CASE("SemanticAnalyzer - Multi Class Inheritance Test")
{
    std::string sourceCode = R"(
        class Base1 {}
        class Base2 {}
        class Child : Base1, Base2 {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-multi-class-inherit");
}

TEST_CASE("SemanticAnalyzer - Funcdef Must Be Used As Handle Test")
{
    std::string sourceCode = R"(
        funcdef void Callback();
        Callback cbNoHandle;
        Callback@ cbWithHandle;
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-funcdef-not-handle");
}

TEST_CASE("SemanticAnalyzer - Typedef Unresolved Base Test")
{
    std::string sourceCode = "typedef UnknownBase MyType;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-typedef-unresolved");
}

TEST_CASE("SemanticAnalyzer - Handle On Primitive In Array And Function Param Test")
{
    std::string sourceCode = R"(
        array<int@> arr1;
        int@[]@ arr2;
        void AnotherFunction(int@ a) {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 3);
    CHECK(diagnostics[0].code == "as-err-handle-on-primitive");
    CHECK(diagnostics[1].code == "as-err-handle-on-primitive");
    CHECK(diagnostics[2].code == "as-err-handle-on-primitive");
}

TEST_CASE("SemanticAnalyzer - Valid Array Handle Test")
{
    std::string sourceCode = R"(
        array<int>@ arr1;
        int[]@ arr2;
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Duplicate Function With Primitive Handle Param Test")
{
    std::string sourceCode = R"(
        void AnotherFunction(int@ a) {}
        void AnotherFunction(int@ a) {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 3);
    CHECK(diagnostics[0].code == "as-err-duplicate-symbol");
    CHECK(diagnostics[1].code == "as-err-handle-on-primitive");
    CHECK(diagnostics[2].code == "as-err-handle-on-primitive");
}

TEST_CASE("SemanticAnalyzer - Function Redefinition With Int And IntHandle Duplicate Test")
{
    std::string sourceCode = R"(
        void AnotherFunction(int a) {}
        void AnotherFunction(int@ a) {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-duplicate-symbol");
    CHECK(diagnostics[1].code == "as-err-handle-on-primitive");
    CHECK(diagnostics[1].range.start.line == 2);
}

TEST_CASE("SemanticAnalyzer - Unresolved Template Type Name Test")
{
    std::string sourceCode = "arar<int> arr3;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-unresolved-type");
}

TEST_CASE("SemanticAnalyzer - Function Redefinition Duplicate Test")
{
    std::string sourceCode = R"(
        void AnotherFunction(int a) {}
        void AnotherFunction(int a) {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-duplicate-symbol");
}

TEST_CASE("SemanticAnalyzer - Function Overload Allowed Test")
{
    std::string sourceCode = R"(
        void AnotherFunction(int a) {}
        void AnotherFunction(float a) {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    CHECK(diagnostics.empty());
}

TEST_CASE("SemanticAnalyzer - Undeclared Class Handle Return Test")
{
    std::string sourceCode = R"(
        Object@ AnotherFunction(int a, float b) {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-unresolved-type");
}

TEST_CASE("SemanticAnalyzer - Funcdef Return Type Without Handle Test")
{
    std::string sourceCode = R"(
        funcdef void SomeFuncDef();
        SomeFuncDef AnotherFunction() {}
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-funcdef-not-handle");
}

TEST_CASE("SemanticAnalyzer - Duplicate Funcdef Variables Test")
{
    std::string sourceCode = R"(
        funcdef void SomeFuncDef();
        SomeFuncDef someVar;
        SomeFuncDef@ someVar;
    )";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{table, fileUri};
    auto diagnostics = analyzer.Analyze(req);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-duplicate-symbol");
    CHECK(diagnostics[1].code == "as-err-funcdef-not-handle");
}

