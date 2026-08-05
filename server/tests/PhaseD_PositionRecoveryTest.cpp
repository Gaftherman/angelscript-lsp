#include <doctest/doctest.h>
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include "i18n/i18n.h"
#include <algorithm>
#include <vector>
#include <string>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace {

struct PositionTestResult
{
    std::vector<Diagnostic> syntaxDiagnostics;
    std::vector<Diagnostic> semanticDiagnostics;
    std::vector<Diagnostic> allDiagnostics;
    SymbolTable table;

    PositionTestResult() = default;
    PositionTestResult(const PositionTestResult &) = delete;
    PositionTestResult &operator=(const PositionTestResult &) = delete;
    PositionTestResult(PositionTestResult &&) = default;
    PositionTestResult &operator=(PositionTestResult &&) = default;

    bool HasCode(const std::string &code) const
    {
        for (const auto &d : allDiagnostics)
        {
            if (d.code == code) return true;
        }
        return false;
    }

    bool HasCodeAtLine(const std::string &code, uint32_t line) const
    {
        for (const auto &d : allDiagnostics)
        {
            if (d.code == code && d.range.start.line == line) return true;
        }
        return false;
    }

    const Diagnostic* FindDiagnostic(const std::string &code) const
    {
        for (const auto &d : allDiagnostics)
        {
            if (d.code == code) return &d;
        }
        return nullptr;
    }

    const Diagnostic* FindDiagnosticAtLine(const std::string &code, uint32_t line) const
    {
        for (const auto &d : allDiagnostics)
        {
            if (d.code == code && d.range.start.line == line) return &d;
        }
        return nullptr;
    }
};

static void RunPositionAnalysis(const std::string &sourceCode, PositionTestResult &res, const std::string &fileUri = "file:///fased_position_test.as")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    angel_lsp::i18n::I18n i18n("en");

    res.syntaxDiagnostics = collector.CollectSymbols(fileUri, sourceCode, parser, res.table, &i18n);

    SemanticAnalyzer analyzer;
    SemanticAnalysisRequest req{res.table, fileUri, "", &i18n};
    res.semanticDiagnostics = analyzer.Analyze(req);

    res.allDiagnostics.insert(res.allDiagnostics.end(), res.syntaxDiagnostics.begin(), res.syntaxDiagnostics.end());
    res.allDiagnostics.insert(res.allDiagnostics.end(), res.semanticDiagnostics.begin(), res.semanticDiagnostics.end());
}

} // namespace

TEST_CASE("Phase D - Position Recovery: Valid Symbol Range Extraction")
{
    SUBCASE("Single global variable position tracking")
    {
        std::string code = "int myGlobalVar = 42;\n";
        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.allDiagnostics.empty());
        REQUIRE(res.table.HasSymbol("myGlobalVar"));

        auto syms = res.table.FindSymbols("myGlobalVar");
        REQUIRE(syms.size() == 1);
        CHECK(syms[0].startLine == 0);
        CHECK(syms[0].startCharacter == 0);
    }

    SUBCASE("Multi-line file symbol start lines")
    {
        std::string code =
            "int varFirst = 1;\n"        // Line 0
            "float varSecond = 2.0f;\n"  // Line 1
            "string varThird = \"hi\";\n"; // Line 2

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.allDiagnostics.empty());

        REQUIRE(res.table.HasSymbol("varFirst"));
        REQUIRE(res.table.HasSymbol("varSecond"));
        REQUIRE(res.table.HasSymbol("varThird"));

        CHECK(res.table.FindSymbols("varFirst")[0].startLine == 0);
        CHECK(res.table.FindSymbols("varSecond")[0].startLine == 1);
        CHECK(res.table.FindSymbols("varThird")[0].startLine == 2);
    }

    SUBCASE("Class with member methods position tracking")
    {
        std::string code =
            "class MyClass {\n"        // Line 0
            "    void MethodOne() {}\n" // Line 1
            "    void MethodTwo() {}\n" // Line 2
            "}\n";

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.allDiagnostics.empty());
        REQUIRE(res.table.HasSymbol("MyClass"));
        REQUIRE(res.table.HasSymbol("MyClass::MethodOne"));
        REQUIRE(res.table.HasSymbol("MyClass::MethodTwo"));

        CHECK(res.table.FindSymbols("MyClass")[0].startLine == 0);
        CHECK(res.table.FindSymbols("MyClass::MethodOne")[0].startLine == 1);
        CHECK(res.table.FindSymbols("MyClass::MethodTwo")[0].startLine == 2);
    }
}

TEST_CASE("Phase D - Position Recovery: Semantic Diagnostic Range Accuracy")
{
    SUBCASE("Break outside loop position accuracy")
    {
        std::string code =
            "void testFunc() {\n"     // Line 0
            "    int a = 10;\n"       // Line 1
            "    break;\n"            // Line 2 -> Error expected at line 2
            "}\n";                    // Line 3

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.HasCode("as-err-break-outside-loop"));
        
        const auto *diag = res.FindDiagnostic("as-err-break-outside-loop");
        REQUIRE(diag != nullptr);
        CHECK(diag->range.start.line == 2);
    }

    SUBCASE("Continue outside loop position accuracy")
    {
        std::string code =
            "void testFunc() {\n"     // Line 0
            "    continue;\n"         // Line 1 -> Error expected at line 1
            "}\n";                    // Line 2

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.HasCode("as-err-continue-outside-loop"));
        
        const auto *diag = res.FindDiagnostic("as-err-continue-outside-loop");
        REQUIRE(diag != nullptr);
        CHECK(diag->range.start.line == 1);
    }

    SUBCASE("Void variable declaration position accuracy")
    {
        std::string code =
            "int validVar = 5;\n"     // Line 0
            "void invalidVoidVar;\n"; // Line 1 -> Error expected at line 1

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.HasCode("as-err-void-variable"));

        const auto *diag = res.FindDiagnostic("as-err-void-variable");
        REQUIRE(diag != nullptr);
        CHECK(diag->range.start.line == 1);
    }

    SUBCASE("Reserved keyword as function name position accuracy")
    {
        std::string code =
            "void validFunc() {}\n"  // Line 0
            "void class() {}\n";     // Line 1 -> Error expected at line 1

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.HasCode("as-err-reserved-keyword-name"));

        const auto *diag = res.FindDiagnostic("as-err-reserved-keyword-name");
        REQUIRE(diag != nullptr);
        CHECK(diag->range.start.line == 1);
    }

    SUBCASE("Base class not found position accuracy")
    {
        std::string code =
            "class ValidBase {}\n"            // Line 0
            "class Derived : UnknownBase {}\n"; // Line 1 -> Error expected at line 1

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.HasCode("as-err-base-not-found"));

        const auto *diag = res.FindDiagnostic("as-err-base-not-found");
        REQUIRE(diag != nullptr);
        CHECK(diag->range.start.line == 1);
    }
}

TEST_CASE("Fase D - Position Recovery: Multi-Error Position Mapping")
{
    SUBCASE("Multiple distinct errors reported on correct respective lines")
    {
        std::string code =
            "void f1() { break; }\n"       // Line 0 -> break outside loop
            "void f2() {}\n"               // Line 1 -> valid
            "void f3() { continue; }\n"    // Line 2 -> continue outside loop
            "void f4() {}\n"               // Line 3 -> valid
            "void class() {}\n";           // Line 4 -> reserved keyword name

        PositionTestResult res;
        RunPositionAnalysis(code, res);

        CHECK(res.HasCodeAtLine("as-err-break-outside-loop", 0));
        CHECK(res.HasCodeAtLine("as-err-continue-outside-loop", 2));
        CHECK(res.HasCodeAtLine("as-err-reserved-keyword-name", 4));
    }

    SUBCASE("Duplicate symbol declaration position mapping")
    {
        std::string code =
            "int item = 1;\n"    // Line 0
            "float item = 2.0;\n"; // Line 1 -> duplicate item

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.HasCode("as-err-duplicate-symbol"));

        const auto *diag = res.FindDiagnostic("as-err-duplicate-symbol");
        REQUIRE(diag != nullptr);
        CHECK(diag->range.start.line == 1);
    }
}

TEST_CASE("Fase D - Position Recovery: Symbol Collection Resilience Under Syntax Distortions")
{
    SUBCASE("Valid symbols collected despite preceding syntax errors in class")
    {
        std::string code =
            "class BrokenClass {\n"     // Line 0
            "    int invalid syntax;\n" // Line 1 -> syntax error inside class
            "}\n"                       // Line 2
            "void validFunction() {}\n"; // Line 3 -> valid top-level function

        PositionTestResult res;
        RunPositionAnalysis(code, res);

        // Valid function following broken class should be collected in SymbolTable
        CHECK(res.table.HasSymbol("validFunction"));
        if (res.table.HasSymbol("validFunction"))
        {
            CHECK(res.table.FindSymbols("validFunction")[0].startLine == 3);
        }
    }

    SUBCASE("Valid class collected despite adjacent top-level syntax noise")
    {
        std::string code = "??? bad top level syntax ???\nclass Vehicle {\n    int speed;\n};\n";
        PositionTestResult res;
        RunPositionAnalysis(code, res);

        CHECK(res.table.HasSymbol("Vehicle"));
        CHECK(res.table.HasSymbol("Vehicle::speed"));
    }
}

TEST_CASE("Phase D - Position Recovery: Advanced Control Flow & Nested Scope Recovery")
{
    SUBCASE("Switch case error position localization")
    {
        std::string code =
            "void testSwitch(int val) {\n"  // Line 0
            "    switch(val) {\n"           // Line 1
            "        case 1: break;\n"      // Line 2
            "        case 1: break;\n"      // Line 3 -> duplicate case value
            "    }\n"                       // Line 4
            "}\n";                          // Line 5

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.HasCode("as-err-duplicate-case-value"));

        const auto *diag = res.FindDiagnostic("as-err-duplicate-case-value");
        REQUIRE(diag != nullptr);
        CHECK(diag->range.start.line == 3);
    }

    SUBCASE("Inout primitive parameter position localization")
    {
        std::string code =
            "void process(int &inout param) {}\n"; // Line 0 -> inout on primitive

        PositionTestResult res;
        RunPositionAnalysis(code, res);
        CHECK(res.HasCode("as-err-inout-on-primitive"));

        const auto *diag = res.FindDiagnostic("as-err-inout-on-primitive");
        REQUIRE(diag != nullptr);
        CHECK(diag->range.start.line == 0);
    }
}

TEST_CASE("Phase E - Incremental Reparse Equivalence")
{
    SUBCASE("Incremental reparse after ts_tree_edit matches fresh parse")
    {
        AngelScriptParser parser;
        std::string v1 = "void f() {}\n";
        std::string v2 = "int x;\nvoid f() {}\n";

        TSTree *tree1 = parser.Parse(v1);
        REQUIRE(tree1 != nullptr);

        TSInputEdit edit;
        edit.start_byte = 0;
        edit.old_end_byte = 0;
        edit.new_end_byte = 7; // strlen("int x;\n")
        edit.start_point = {0, 0};
        edit.old_end_point = {0, 0};
        edit.new_end_point = {1, 0};
        ts_tree_edit(tree1, &edit);

        TSTree *tree2 = parser.Parse(v2, tree1);
        TSTree *tree3 = parser.Parse(v2);
        REQUIRE(tree2 != nullptr);
        REQUIRE(tree3 != nullptr);

        TSNode root2 = ts_tree_root_node(tree2);
        TSNode root3 = ts_tree_root_node(tree3);

        char *sexp2 = ts_node_string(root2);
        char *sexp3 = ts_node_string(root3);
        std::string s2 = sexp2;
        std::string s3 = sexp3;
        free(sexp2);
        free(sexp3);

        CHECK(s2 == s3);
        CHECK(!ts_node_has_error(root2));

        ts_tree_delete(tree1);
        ts_tree_delete(tree2);
        ts_tree_delete(tree3);
    }

    SUBCASE("Edit inside function body reparse equivalence")
    {
        AngelScriptParser parser;
        std::string v1 = "void f() {\n    int a = 1;\n}\n";
        std::string v2 = "void f() {\n    int a = 123;\n}\n";

        TSTree *tree1 = parser.Parse(v1);
        REQUIRE(tree1 != nullptr);

        // Replace "1" with "123" at byte 23 (line 1 starts at byte 11, "1" is at column 12)
        TSInputEdit edit;
        edit.start_byte = 23;
        edit.old_end_byte = 24;
        edit.new_end_byte = 26;
        edit.start_point = {1, 12};
        edit.old_end_point = {1, 13};
        edit.new_end_point = {1, 15};
        ts_tree_edit(tree1, &edit);

        TSTree *tree2 = parser.Parse(v2, tree1);
        TSTree *tree3 = parser.Parse(v2);
        REQUIRE(tree2 != nullptr);
        REQUIRE(tree3 != nullptr);

        char *sexp2 = ts_node_string(ts_tree_root_node(tree2));
        char *sexp3 = ts_node_string(ts_tree_root_node(tree3));
        std::string s2 = sexp2;
        std::string s3 = sexp3;
        free(sexp2);
        free(sexp3);

        CHECK(s2 == s3);

        ts_tree_delete(tree1);
        ts_tree_delete(tree2);
        ts_tree_delete(tree3);
    }
}
