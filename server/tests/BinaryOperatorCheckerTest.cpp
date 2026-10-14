#include <doctest/doctest.h>

#include "analysis/BinaryOperatorChecker.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/EngineProfiles.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::test
{
using namespace angel_lsp::analysis;

namespace
{
std::vector<Diagnostic> AnalyzeScriptWithStandardProfile(const std::string& code)
{
    const std::string fileUri = "file:///" + GenerateRandomSymbolName() + ".as";
    parser::AngelScriptParser parser;
    analysis::SymbolCollector collector(nullptr);
    analysis::LocalScopeCollector scopes(nullptr);
    analysis::SymbolTable table;
    static i18n::I18n i18n;

    // Load standard built-in profile (defines string, ref, array, etc.)
    const std::string stdStub = GetProfileStubText(EngineProfileKind::Standard);
    collector.CollectSymbols(GetProfileSyntheticUri(EngineProfileKind::Standard), stdStub, parser, table);

    // Collect user script symbols
    collector.CollectSymbols(fileUri, code, parser, table);

    analysis::SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    request.tree = parser.Parse(code);

    analysis::SemanticAnalyzer analyzer(nullptr);
    auto diagnostics = analyzer.Analyze(request);

    if (request.tree)
    {
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    }
    return diagnostics;
}

bool HasDiagnostic(const std::vector<Diagnostic>& diagnostics, std::string_view code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_SUITE("BinaryOperatorChecker")
{
    TEST_CASE("Bitwise OR between integer and string triggers as-err-no-matching-operator")
    {
        const std::string varName = GenerateRandomSymbolName();
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    int " +
                                 varName +
                                 " = 5 | \"hello\";\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Bitwise OR with const int and string literal in function argument triggers error")
    {
        const std::string constName = GenerateRandomSymbolName();
        const std::string fnName = GenerateRandomSymbolName();
        const std::string callerName = GenerateRandomSymbolName();
        const std::string code = "const int " + constName +
                                 " = 1024;\n"
                                 "void " +
                                 fnName +
                                 "(int a, int b) {}\n"
                                 "void " +
                                 callerName +
                                 "() {\n"
                                 "    " +
                                 fnName + "(1, (" + constName +
                                 " | \"true\"));\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Valid bitwise operations between integers produce no diagnostic")
    {
        const std::string v1 = GenerateRandomSymbolName();
        const std::string v2 = GenerateRandomSymbolName();
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    int " +
                                 v1 +
                                 " = 5 | 3;\n"
                                 "    uint " +
                                 v2 +
                                 " = (10 & 2) ^ (1 << 4);\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Bitwise operations with enums produce no diagnostic")
    {
        const std::string enumName = "E" + GenerateRandomSymbolName();
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "enum " + enumName +
                                 " { A = 1, B = 2 }\n"
                                 "void " +
                                 fnName +
                                 "() {\n"
                                 "    int flags = " +
                                 enumName + "::A | " + enumName +
                                 "::B;\n"
                                 "    int combined = " +
                                 enumName +
                                 "::A | 4;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Bitwise OR on bool triggers as-err-no-matching-operator")
    {
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    bool b = true | false;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Modulo with float triggers as-err-no-matching-operator")
    {
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    float f = 5.0f % 2.0f;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("String subtraction triggers as-err-no-matching-operator")
    {
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    string s = \"hello\" - 5;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("String concatenation with int is valid and triggers no operator error")
    {
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    string s = \"hello\" + 5;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE(
        "BinaryOperator - Reverse operator overload exact match takes precedence over forward conversion (Probe A)")
    {
        const std::string leftClass = GenerateRandomSymbolName("LeftClass");
        const std::string rightClass = GenerateRandomSymbolName("RightClass");
        const std::string fnName = GenerateRandomSymbolName("TestProbeA1");
        const std::string code = "class " + rightClass + " {}\n" + "class " + leftClass + " {\n" +
                                 "    int opAdd(double d) const { return 1; }\n" + "}\n" + "class " + rightClass +
                                 "_WithRev {\n" + "    int opAdd_r(const " + leftClass +
                                 " &in l) const { return 2; }\n" + "}\n" + "void " + fnName + "() {\n" + "    " +
                                 leftClass + " l;\n" + "    " + rightClass + "_WithRev r;\n" +
                                 "    int res = l + r;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE(
        "BinaryOperator - Left operator takes precedence when both forward and reverse are exact matches (Probe A)")
    {
        const std::string leftClass = GenerateRandomSymbolName("LeftExact");
        const std::string rightClass = GenerateRandomSymbolName("RightExact");
        const std::string fnName = GenerateRandomSymbolName("TestProbeA2");
        const std::string code = "class " + rightClass + ";\n" + "class " + leftClass + " {\n" +
                                 "    int opAdd(const " + rightClass + " &in r) const { return 1; }\n" + "}\n" +
                                 "class " + rightClass + " {\n" + "    int opAdd_r(const " + leftClass +
                                 " &in l) const { return 2; }\n" + "}\n" + "void " + fnName + "() {\n" + "    " +
                                 leftClass + " l;\n" + "    " + rightClass + " r;\n" + "    int res = l + r;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperator - Single-side opImplConv with primitive operand succeeds (Probe B)")
    {
        const std::string clsName = GenerateRandomSymbolName("ConvClass");
        const std::string fnName = GenerateRandomSymbolName("TestProbeB1");
        const std::string code = "class " + clsName + " {\n" + "    int opImplConv() const { return 42; }\n" + "}\n" +
                                 "void " + fnName + "() {\n" + "    " + clsName + " a;\n" + "    int x = a + 10;\n" +
                                 "    int y = 10 + a;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperator - Dual-class opImplConv without primitive operand is rejected (Probe B)")
    {
        const std::string clsA = GenerateRandomSymbolName("ClassA");
        const std::string clsB = GenerateRandomSymbolName("ClassB");
        const std::string fnName = GenerateRandomSymbolName("TestProbeB2");
        const std::string code = "class " + clsA + " {\n" + "    int opImplConv() const { return 1; }\n" + "}\n" +
                                 "class " + clsB + " {\n" + "    int opImplConv() const { return 2; }\n" + "}\n" +
                                 "void " + fnName + "() {\n" + "    " + clsA + " a;\n" + "    " + clsB + " b;\n" +
                                 "    int x = a + b;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperator - Enum bitwise operations evaluate to int")
    {
        const std::string enumName = GenerateRandomSymbolName("TestFlags");
        const std::string fnName = GenerateRandomSymbolName("TestEnumBitwise");
        const std::string code = "enum " + enumName + " { A = 1, B = 2 }\n" + "void " + fnName + "() {\n" +
                                 "    int x = " + enumName + "::A | " + enumName + "::B;\n" +
                                 "    int y = " + enumName + "::A & " + enumName + "::B;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoImplicitConversion));
    }
}
} // namespace angel_lsp::test
