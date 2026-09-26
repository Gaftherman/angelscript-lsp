#include <doctest/doctest.h>

#include "analysis/ComparisonOperatorChecker.h"
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
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_SUITE("ComparisonOperatorChecker")
{
    TEST_CASE("Incompatible Equality Comparison: string == int and string == bool")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        const std::string script =
            "void " + funcName + "()\n" +
            "{\n" +
            "    string " + varName + " = \"test\";\n" +
            "    if (" + varName + " == 1) {}\n" +
            "    if (1 == " + varName + ") {}\n" +
            "    if (" + varName + " == true) {}\n" +
            "    if (false == " + varName + ") {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        size_t count = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::NoMatchingOperator)
            {
                ++count;
                CHECK(d.severity == DiagnosticSeverity::Error);
            }
        }
        CHECK(count == 4);
    }

    TEST_CASE("Incompatible Relational Comparison: string <= int and string >= bool")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        const std::string script =
            "void " + funcName + "()\n" +
            "{\n" +
            "    string " + varName + " = \"test\";\n" +
            "    if (" + varName + " <= 1) {}\n" +
            "    if (1 < " + varName + ") {}\n" +
            "    if (" + varName + " >= true) {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Relational Comparison on Boolean Emits IllegalOperation")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string b1 = GenerateRandomSymbolName();
        const std::string b2 = GenerateRandomSymbolName();

        const std::string script =
            "void " + funcName + "(bool " + b1 + ", bool " + b2 + ")\n" +
            "{\n" +
            "    if (" + b1 + " < " + b2 + ") {}\n" +
            "    if (" + b1 + " <= true) {}\n" +
            "    if (false >= " + b2 + ") {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        size_t illegalCount = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::IllegalOperation)
            {
                ++illegalCount;
                CHECK(d.severity == DiagnosticSeverity::Error);
            }
        }
        CHECK(illegalCount == 3);
    }

    TEST_CASE("Compatible Comparisons Emit Zero Diagnostics")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string s1 = GenerateRandomSymbolName();
        const std::string s2 = GenerateRandomSymbolName();
        const std::string n1 = GenerateRandomSymbolName();
        const std::string n2 = GenerateRandomSymbolName();

        const std::string script =
            "void " + funcName + "(string " + s1 + ", string " + s2 + ", int " + n1 + ", float " + n2 + ")\n" +
            "{\n" +
            "    if (" + s1 + " == " + s2 + ") {}\n" +
            "    if (" + s1 + " != \"literal\") {}\n" +
            "    if (" + s1 + " <= " + s2 + ") {}\n" +
            "    if (" + n1 + " == 10) {}\n" +
            "    if (" + n1 + " < " + n2 + ") {}\n" +
            "    if (true == false) {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::IllegalOperation));
    }

    TEST_CASE("Implicit Conversion with opImplConv: string_t to string is Valid")
    {
        const std::string typeName = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string strVar = GenerateRandomSymbolName();
        const std::string convVar = GenerateRandomSymbolName();

        const std::string script =
            "class " + typeName + "\n" +
            "{\n" +
            "    string opImplConv() const { return \"\"; }\n" +
            "}\n" +
            "void " + funcName + "(string " + strVar + ", " + typeName + " " + convVar + ")\n" +
            "{\n" +
            "    if (" + strVar + " == " + convVar + ") {}\n" +
            "    if (" + convVar + " == " + strVar + ") {}\n" +
            "    if (" + strVar + " <= " + convVar + ") {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Generic ref Equality vs Relational Operators")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string strVar = GenerateRandomSymbolName();
        const std::string refVar = GenerateRandomSymbolName();

        const std::string script =
            "void " + funcName + "(string " + strVar + ", ref@ " + refVar + ")\n" +
            "{\n" +
            "    if (" + strVar + " == " + refVar + ") {}\n" + // Valid via ref::opEquals(?&)
            "    if (" + strVar + " <= " + refVar + ") {}\n" + // Invalid: ref has no opCmp
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        size_t count = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::NoMatchingOperator)
            {
                ++count;
            }
        }
        CHECK(count == 1);
    }

    TEST_CASE("Compound Logical Expressions with Multiple Errors")
    {
        const std::string otherClass = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string strVar = GenerateRandomSymbolName();
        const std::string otherVar = GenerateRandomSymbolName();

        const std::string script =
            "class " + otherClass + " {}\n" +
            "void " + funcName + "(string " + strVar + ", " + otherClass + "@ " + otherVar + ")\n" +
            "{\n" +
            "    if ((" + strVar + " == 1) || " + strVar + " == true || " + strVar + " == " + otherVar + ") {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        size_t count = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::NoMatchingOperator)
            {
                ++count;
            }
        }
        // Exactly 3 errors: string == int, string == bool, string == otherClass
        CHECK(count == 3);
    }

    TEST_CASE("Custom Class with opEquals and opCmp Passes Cleanly")
    {
        const std::string cls1 = GenerateRandomSymbolName();
        const std::string cls2 = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string v1 = GenerateRandomSymbolName();
        const std::string v2 = GenerateRandomSymbolName();

        const std::string script =
            "class " + cls2 + " {}\n" +
            "class " + cls1 + "\n" +
            "{\n" +
            "    bool opEquals(const " + cls2 + " &in other) const { return true; }\n" +
            "    int opCmp(const " + cls2 + " &in other) const { return 0; }\n" +
            "}\n" +
            "void " + funcName + "(" + cls1 + " " + v1 + ", " + cls2 + " " + v2 + ")\n" +
            "{\n" +
            "    if (" + v1 + " == " + v2 + ") {}\n" +
            "    if (" + v1 + " <= " + v2 + ") {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Increment and Decrement in Conditions Are Validated Cleanly (asharness parity)")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string script =
            "void " + funcName + "()\n" +
            "{\n" +
            "    int i = 0;\n" +
            "    if (i++ == 0) {}\n" +
            "    if (++i == 1) {}\n" +
            "    if (--i == -1) {}\n" +
            "    if (i-- == 0) {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        CHECK(diags.empty());
    }

    TEST_CASE("Overloaded Unary Operators in Conditions: opPostInc, opPreInc, opNeg, opCom (asharness parity)")
    {
        const std::string clsName = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        const std::string script =
            "class " + clsName + "\n" +
            "{\n" +
            "    int val = 0;\n" +
            "    " + clsName + " opPostInc() { " + clsName + " old = this; val++; return old; }\n" +
            "    " + clsName + "& opPreInc() { val++; return this; }\n" +
            "    " + clsName + " opNeg() { " + clsName + " n; n.val = -val; return n; }\n" +
            "    " + clsName + " opCom() { " + clsName + " n; n.val = ~val; return n; }\n" +
            "    bool opEquals(int other) const { return val == other; }\n" +
            "}\n" +
            "void " + funcName + "()\n" +
            "{\n" +
            "    " + clsName + " " + varName + ";\n" +
            "    if (" + varName + "++ == 0) {}\n" +
            "    if (++" + varName + " == 1) {}\n" +
            "    if (-" + varName + " == -5) {}\n" +
            "    if (~" + varName + " == 0) {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Logical Keywords not, and, or, xor in Conditions (asharness parity)")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string script =
            "void " + funcName + "()\n" +
            "{\n" +
            "    bool a = true;\n" +
            "    bool b = false;\n" +
            "    if (not a) {}\n" +
            "    if (a and b) {}\n" +
            "    if (a or b) {}\n" +
            "    if (a xor b) {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(script);
        CHECK(diags.empty());
    }
}

} // namespace angel_lsp::test
