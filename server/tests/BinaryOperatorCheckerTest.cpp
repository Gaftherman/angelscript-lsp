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
}
} // namespace angel_lsp::test
