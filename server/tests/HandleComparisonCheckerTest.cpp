#include <doctest/doctest.h>

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
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
std::vector<Diagnostic> AnalyzeScript(const std::string& code,
                                      const config::DiagnosticsConfig* diagConfig = nullptr)
{
    const std::string fileUri = "file:///" + GenerateRandomSymbolName() + ".as";
    parser::AngelScriptParser parser;
    analysis::SymbolCollector collector(nullptr);
    analysis::LocalScopeCollector scopes(nullptr);
    analysis::SymbolTable table;
    static i18n::I18n i18n;

    collector.CollectSymbols(fileUri, code, parser, table);

    analysis::SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    request.tree = parser.Parse(code);
    request.diagnostics = diagConfig;

    analysis::SemanticAnalyzer analyzer(nullptr);
    auto diagnostics = analyzer.Analyze(request);

    if (request.tree)
    {
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    }
    return diagnostics;
}

bool HasDiagnosticWithSeverity(const std::vector<Diagnostic>& diagnostics, std::string_view code,
                               DiagnosticSeverity severity)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d) { return d.code == code && d.severity == severity; });
}

bool HasDiagnosticCode(const std::vector<Diagnostic>& diagnostics, std::string_view code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_SUITE("HandleComparisonChecker")
{
    TEST_CASE("Handle Equality Comparison with Null Emits Warning by Default")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        const std::string script = "class " + className + " {}\n" +
                                   "void " + funcName + "(" + className + "@ " + varName + ")\n" +
                                   "{\n" +
                                   "    if (" + varName + " == null) {}\n" +
                                   "    if (null == " + varName + ") {}\n" +
                                   "    if (" + varName + " != null) {}\n" +
                                   "    if (null != " + varName + ") {}\n" +
                                   "}\n";

        auto diags = AnalyzeScript(script);
        CHECK(HasDiagnosticWithSeverity(diags, diagnostics::codes::HandleComparisonEquality,
                                        DiagnosticSeverity::Warning));

        size_t count = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::HandleComparisonEquality)
            {
                ++count;
            }
        }
        CHECK(count == 4);
    }

    TEST_CASE("Handle Identity Comparison with Null Emits Zero Warnings")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        const std::string script = "class " + className + " {}\n" +
                                   "void " + funcName + "(" + className + "@ " + varName + ")\n" +
                                   "{\n" +
                                   "    if (" + varName + " is null) {}\n" +
                                   "    if (null is " + varName + ") {}\n" +
                                   "    if (" + varName + " !is null) {}\n" +
                                   "    if (null !is " + varName + ") {}\n" +
                                   "}\n";

        auto diags = AnalyzeScript(script);
        CHECK_FALSE(HasDiagnosticCode(diags, diagnostics::codes::HandleComparisonEquality));
    }

    TEST_CASE("Configurable Severity: Error Mode (2)")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        config::DiagnosticsConfig cfg;
        cfg.reportHandleComparisonEquality = 2;

        const std::string script = "class " + className + " {}\n" +
                                   "void " + funcName + "(" + className + "@ " + varName + ")\n" +
                                   "{\n" +
                                   "    if (" + varName + " == null) {}\n" +
                                   "}\n";

        auto diags = AnalyzeScript(script, &cfg);
        CHECK(HasDiagnosticWithSeverity(diags, diagnostics::codes::HandleComparisonEquality,
                                        DiagnosticSeverity::Error));
    }

    TEST_CASE("Configurable Severity: Disabled Mode (0)")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        config::DiagnosticsConfig cfg;
        cfg.reportHandleComparisonEquality = 0;

        const std::string script = "class " + className + " {}\n" +
                                   "void " + funcName + "(" + className + "@ " + varName + ")\n" +
                                   "{\n" +
                                   "    if (" + varName + " == null) {}\n" +
                                   "    if (" + varName + " != null) {}\n" +
                                   "}\n";

        auto diags = AnalyzeScript(script, &cfg);
        CHECK_FALSE(HasDiagnosticCode(diags, diagnostics::codes::HandleComparisonEquality));
    }

    TEST_CASE("Relational Comparisons with Null Are Illegal Operations")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        const std::string script = "class " + className + " {}\n" +
                                   "void " + funcName + "(" + className + "@ " + varName + ")\n" +
                                   "{\n" +
                                   "    if (" + varName + " <= null) {}\n" +
                                   "    if (" + varName + " < null) {}\n" +
                                   "    if (" + varName + " >= null) {}\n" +
                                   "    if (" + varName + " > null) {}\n" +
                                   "    if (null <= " + varName + ") {}\n" +
                                   "    if (null < " + varName + ") {}\n" +
                                   "    if (null >= " + varName + ") {}\n" +
                                   "    if (null > " + varName + ") {}\n" +
                                   "}\n";

        auto diags = AnalyzeScript(script);
        CHECK(HasDiagnosticWithSeverity(diags, diagnostics::codes::IllegalOperation,
                                        DiagnosticSeverity::Error));

        size_t count = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::IllegalOperation)
            {
                ++count;
            }
        }
        CHECK(count == 8);
    }

    TEST_CASE("Relational Comparison Between Handles Without opCmp Is Illegal")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string var1 = GenerateRandomSymbolName();
        const std::string var2 = GenerateRandomSymbolName();

        const std::string script = "class " + className + " {}\n" +
                                   "void " + funcName + "(" + className + "@ " + var1 + ", " + className + "@ " + var2 + ")\n" +
                                   "{\n" +
                                   "    if (" + var1 + " <= " + var2 + ") {}\n" +
                                   "    if (" + var1 + " < " + var2 + ") {}\n" +
                                   "    if (" + var1 + " >= " + var2 + ") {}\n" +
                                   "    if (" + var1 + " > " + var2 + ") {}\n" +
                                   "}\n";

        auto diags = AnalyzeScript(script);
        CHECK(HasDiagnosticWithSeverity(diags, diagnostics::codes::IllegalOperation,
                                        DiagnosticSeverity::Error));

        size_t count = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::IllegalOperation)
            {
                ++count;
            }
        }
        CHECK(count == 4);
    }

    TEST_CASE("Relational Comparison Between Handles With opCmp Is Permitted")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string var1 = GenerateRandomSymbolName();
        const std::string var2 = GenerateRandomSymbolName();

        const std::string script = "class " + className + "\n" +
                                   "{\n" +
                                   "    int opCmp(const " + className + " &in other) const { return 0; }\n" +
                                   "}\n" +
                                   "void " + funcName + "(" + className + "@ " + var1 + ", " + className + "@ " + var2 + ")\n" +
                                   "{\n" +
                                   "    if (" + var1 + " <= " + var2 + ") {}\n" +
                                   "    if (" + var1 + " < " + var2 + ") {}\n" +
                                   "    if (" + var1 + " >= " + var2 + ") {}\n" +
                                   "    if (" + var1 + " > " + var2 + ") {}\n" +
                                   "}\n";

        auto diags = AnalyzeScript(script);
        CHECK_FALSE(HasDiagnosticCode(diags, diagnostics::codes::IllegalOperation));
    }

    TEST_CASE("Short-Circuit Logic and Flow Assertions")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string methodName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        const std::string script =
            "class " + className + "\n" +
            "{\n" +
            "    bool " + methodName + "() { return true; }\n" +
            "}\n" +
            "void " + funcName + "(" + className + "@ " + varName + ")\n" +
            "{\n" +
            "    if (true || " + varName + " == null) {}\n" +
            "    if (" + varName + " is null || " + varName + "." + methodName + "()) {}\n" +
            "    if (" + varName + " !is null && " + varName + "." + methodName + "()) {}\n" +
            "    bool flag = (" + varName + " != null) ? true : false;\n" +
            "    while (" + varName + " == null) {}\n" +
            "}\n";

        auto diags = AnalyzeScript(script);
        // The first if, the ternary condition, and while statement have equality comparisons with null
        CHECK(HasDiagnosticCode(diags, diagnostics::codes::HandleComparisonEquality));
        // None of the short-circuited method calls dereference an unchecked null handle
        CHECK_FALSE(HasDiagnosticCode(diags, diagnostics::codes::PossibleNullDereference));

        size_t equalityWarnings = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::HandleComparisonEquality)
            {
                ++equalityWarnings;
            }
        }
        CHECK(equalityWarnings == 3);
    }

    TEST_CASE("Array Indexing With Handle Elements: Equality, Identity, and Relational Operations")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string arrName1 = GenerateRandomSymbolName();
        const std::string arrName2 = GenerateRandomSymbolName();

        const std::string script =
            "class " + className + " {}\n" +
            "void " + funcName + "(array<" + className + "@> " + arrName1 + ", array<" + className + "@> " + arrName2 + ")\n" +
            "{\n" +
            "    if (" + arrName1 + "[0] == null) {}\n" +
            "    if (null == " + arrName1 + "[0]) {}\n" +
            "    if (" + arrName1 + "[0] != null) {}\n" +
            "    if (null != " + arrName1 + "[0]) {}\n" +
            "    if (" + arrName1 + "[0] is null) {}\n" +
            "    if (" + arrName1 + "[0] !is null) {}\n" +
            "    if (" + arrName1 + "[0] <= null) {}\n" +
            "    if (null <= " + arrName1 + "[0]) {}\n" +
            "    if (" + arrName1 + "[0] <= " + arrName2 + "[0]) {}\n" +
            "}\n";

        auto diags = AnalyzeScript(script);
        size_t equalityWarnings = 0;
        size_t illegalErrors = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::HandleComparisonEquality)
            {
                ++equalityWarnings;
            }
            else if (d.code == diagnostics::codes::IllegalOperation)
            {
                ++illegalErrors;
            }
        }
        // Exactly 4 equality comparisons with null on array elements emit warnings
        CHECK(equalityWarnings == 4);
        // Exactly 3 relational operations (<= null, null <=, and <= without opCmp) emit illegal operation error
        CHECK(illegalErrors == 3);
    }

    TEST_CASE("Multi-Dimensional Array Handles and Complex Expressions")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string holderName = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string getFuncName = GenerateRandomSymbolName();
        const std::string matrixName = GenerateRandomSymbolName();
        const std::string holderVar = GenerateRandomSymbolName();

        const std::string script =
            "class " + className + " {}\n" +
            "class " + holderName + "\n" +
            "{\n" +
            "    " + className + "@ handleField;\n" +
            "}\n" +
            "" + className + "@ " + getFuncName + "() { return null; }\n" +
            "void " + funcName + "(array<array<" + className + "@>> " + matrixName + ", " + holderName + "@ " + holderVar + ")\n" +
            "{\n" +
            "    if (" + matrixName + "[0][0] == null) {}\n" +
            "    if (" + holderVar + ".handleField == null) {}\n" +
            "    if (" + getFuncName + "() == null) {}\n" +
            "    if ((" + holderVar + ".handleField) != null) {}\n" +
            "}\n";

        auto diags = AnalyzeScript(script);
        size_t equalityWarnings = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::HandleComparisonEquality)
            {
                ++equalityWarnings;
            }
        }
        CHECK(equalityWarnings == 4);
    }

    TEST_CASE("Array of Non-Handle Value Classes and Primitives Do Not Emit Handle Warnings")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string valArr = GenerateRandomSymbolName();
        const std::string intArr = GenerateRandomSymbolName();

        const std::string script =
            "class " + className + " {}\n" +
            "void " + funcName + "(array<" + className + "> " + valArr + ", array<int> " + intArr + ")\n" +
            "{\n" +
            "    if (" + intArr + "[0] <= 5) {}\n" +
            "}\n";

        auto diags = AnalyzeScript(script);
        CHECK_FALSE(HasDiagnosticCode(diags, diagnostics::codes::HandleComparisonEquality));
        CHECK_FALSE(HasDiagnosticCode(diags, diagnostics::codes::IllegalOperation));
    }

    TEST_CASE("Funcdef Function Handles Are Validated Properly")
    {
        const std::string funcdefName = GenerateRandomSymbolName();
        const std::string testFuncName = GenerateRandomSymbolName();
        const std::string cbVar = GenerateRandomSymbolName();

        const std::string script =
            "funcdef void " + funcdefName + "();\n" +
            "void " + testFuncName + "(" + funcdefName + " " + cbVar + ")\n" +
            "{\n" +
            "    if (" + cbVar + " == null) {}\n" +
            "    if (" + cbVar + " != null) {}\n" +
            "    if (" + cbVar + " is null) {}\n" +
            "    if (" + cbVar + " !is null) {}\n" +
            "}\n";

        auto diags = AnalyzeScript(script);
        size_t equalityWarnings = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::HandleComparisonEquality)
            {
                ++equalityWarnings;
            }
        }
        CHECK(equalityWarnings == 2);
    }
}

} // namespace angel_lsp::test
