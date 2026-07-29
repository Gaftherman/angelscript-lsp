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
        SemanticAnalysisRequest req{table, fileUri};
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
        SemanticAnalysisRequest req{table, fileUri};
        auto diagnostics = analyzer.Analyze(req);

        CHECK(diagnostics.empty());
    }
}
