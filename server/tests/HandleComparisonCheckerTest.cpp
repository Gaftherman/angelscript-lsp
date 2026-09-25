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
}

} // namespace angel_lsp::test
