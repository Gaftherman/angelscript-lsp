#include <doctest/doctest.h>

#include "analysis/DiagnosticCodes.h"
#include "analysis/EngineProfiles.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
#include "features/code_action/CodeActionHandler.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace angel_lsp::test
{
using namespace angel_lsp::analysis;
using namespace angel_lsp::config;
using namespace angel_lsp::features;

namespace
{
std::vector<Diagnostic> AnalyzeScriptWithSvenProfile(const std::string& code, const DiagnosticsConfig* diagConfig = nullptr)
{
    const std::string fileUri = "file:///" + GenerateRandomSymbolName() + ".as";
    parser::AngelScriptParser parser;
    analysis::SymbolCollector collector(nullptr);
    analysis::LocalScopeCollector scopes(nullptr);
    analysis::SymbolTable table;
    static i18n::I18n i18n;

    std::filesystem::path repoRoot(ANGELSCRIPT_REPO_ROOT);
    std::filesystem::path stubPath = repoRoot / "predefined" / "sven.as.predefined";
    if (std::filesystem::exists(stubPath))
    {
        std::ifstream stubFile(stubPath, std::ios::binary);
        std::string stubContent((std::istreambuf_iterator<char>(stubFile)), std::istreambuf_iterator<char>());
        collector.CollectSymbols("file:///sven.as.predefined", stubContent, parser, table);
    }
    else
    {
        const std::string svenStub = GetProfileStubText(EngineProfileKind::SvenCoop);
        collector.CollectSymbols(GetProfileSyntheticUri(EngineProfileKind::SvenCoop), svenStub, parser, table);
    }
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

bool HasDiagnostic(const std::vector<Diagnostic>& diagnostics, std::string_view code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_SUITE("RepeatedConversionChecker")
{
    TEST_CASE("Repeated string_t comparison in if-else ladder emits hint")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();

        const std::string script =
            "final class " + className + " : ScriptBaseEntity\n" +
            "{\n" +
            "    void " + funcName + "()\n" +
            "    {\n" +
            "        if (self.pev.target == \"Foo\") {}\n" +
            "        else if (self.pev.target == \"Bar\") {}\n" +
            "    }\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithSvenProfile(script);
        size_t count = 0;
        for (const auto& d : diags)
        {
            if (d.code == diagnostics::codes::RepeatedConversion)
            {
                ++count;
                CHECK(d.severity == DiagnosticSeverity::Hint);
            }
        }
        CHECK(count == 2);
    }

    TEST_CASE("Single string_t comparison emits zero hints")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();

        const std::string script =
            "final class " + className + " : ScriptBaseEntity\n" +
            "{\n" +
            "    void " + funcName + "()\n" +
            "    {\n" +
            "        if (self.pev.target == \"Single\") {}\n" +
            "    }\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithSvenProfile(script);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::RepeatedConversion));
    }

    TEST_CASE("Ternary expression in snprintf argument matches wildcard ?& in cleanly")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string timeVar = GenerateRandomSymbolName();
        const std::string daysVar = GenerateRandomSymbolName();
        const std::string msgVar = GenerateRandomSymbolName();

        const std::string script =
            "void " + funcName + "(string& out " + timeVar + ", int " + daysVar + ", array<string> " + msgVar + ")\n" +
            "{\n" +
            "    if (" + daysVar + " > 0) {\n" +
            "        snprintf(" + timeVar + ", \"%1%2 %3 \", " + timeVar + ", " + daysVar + ", (" + daysVar + " > 1 ? " + msgVar + "[2] : " + msgVar + "[1]));\n" +
            "    }\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithSvenProfile(script);
        CHECK_FALSE(HasDiagnostic(diags, "as-err-no-implicit-conversion"));
    }

    TEST_CASE("Null safety policy: default warns on first dereference, config warns on all")
    {
        const std::string funcName = GenerateRandomSymbolName();
        const std::string paramName = GenerateRandomSymbolName();

        const std::string script =
            "void " + funcName + "(SayParameters@ " + paramName + ")\n" +
            "{\n" +
            "    " + paramName + ".GetArguments();\n" +
            "    " + paramName + ".get_ShouldHide();\n" +
            "}\n";

        // Default configuration: warns only on first dereference
        {
            DiagnosticsConfig cfg;
            cfg.reportAllNullDereferences = false;
            const auto diags = AnalyzeScriptWithSvenProfile(script, &cfg);
            size_t nullWarnings = 0;
            for (const auto& d : diags)
            {
                if (d.code == diagnostics::codes::PossibleNullDereference)
                {
                    ++nullWarnings;
                }
            }
            CHECK(nullWarnings == 1);
        }

        // Configured to report all null dereferences
        {
            DiagnosticsConfig cfg;
            cfg.reportAllNullDereferences = true;
            const auto diags = AnalyzeScriptWithSvenProfile(script, &cfg);
            size_t nullWarnings = 0;
            for (const auto& d : diags)
            {
                if (d.code == diagnostics::codes::PossibleNullDereference)
                {
                    ++nullWarnings;
                }
            }
            CHECK(nullWarnings == 2);
        }
    }

    TEST_CASE("Class with opCmp cleanly satisfies all comparison operators including == and !=")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string v1 = GenerateRandomSymbolName();
        const std::string v2 = GenerateRandomSymbolName();

        const std::string script =
            "class " + className + "\n" +
            "{\n" +
            "    int m_Major;\n" +
            "    int m_Minor;\n" +
            "    int opCmp(const " + className + " &in other) const\n" +
            "    {\n" +
            "        if (this.m_Major != other.m_Major)\n" +
            "            return (this.m_Major < other.m_Major) ? -1 : 1;\n" +
            "        if (this.m_Minor != other.m_Minor)\n" +
            "            return (this.m_Minor < other.m_Minor) ? -1 : 1;\n" +
            "        return 0;\n" +
            "    }\n" +
            "}\n" +
            "void " + funcName + "(" + className + " " + v1 + ", " + className + " " + v2 + ")\n" +
            "{\n" +
            "    if (" + v1 + " == " + v2 + ") {}\n" +
            "    if (" + v1 + " != " + v2 + ") {}\n" +
            "    if (" + v1 + " < " + v2 + ") {}\n" +
            "    if (" + v1 + " <= " + v2 + ") {}\n" +
            "    if (" + v1 + " > " + v2 + ") {}\n" +
            "    if (" + v1 + " >= " + v2 + ") {}\n" +
            "}\n";

        const auto diags = AnalyzeScriptWithSvenProfile(script);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
        CHECK_FALSE(HasDiagnostic(diags, "as-err-no-implicit-conversion"));
    }

    TEST_CASE("Explicit opConv cannot be used for implicit comparison or assignment")
    {
        const std::string className = GenerateRandomSymbolName();
        const std::string funcName = GenerateRandomSymbolName();
        const std::string varName = GenerateRandomSymbolName();

        const std::string script =
            "class " + className + "\n" +
            "{\n" +
            "    int opConv() const { return 0; }\n" +
            "}\n" +
            "void " + funcName + "(" + className + " " + varName + ")\n" +
            "{\n" +
            "    int i = " + varName + ";\n" + // Error: explicit opConv requires cast
            "    if (" + varName + " == 1) {}\n" + // Error: no implicit conversion
            "}\n";

        const auto diags = AnalyzeScriptWithSvenProfile(script);
        CHECK(HasDiagnostic(diags, "as-err-no-implicit-conversion"));
    }
}

} // namespace angel_lsp::test
