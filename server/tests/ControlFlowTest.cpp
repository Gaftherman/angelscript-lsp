#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "analysis/ControlFlowChecker.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    std::vector<Diagnostic> AnalyzeFlowSnippet(const std::string &code,
                                               const std::string &fileUri = "file:///flow.as")
    {
        static AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static angel_lsp::i18n::I18n i18n;

        collector.CollectSymbols(fileUri, code, parser, table);

        SemanticAnalysisRequest request{ table, fileUri, ".as.predefined", &i18n };
        request.scopeRoot = scopes.CollectScopes(code, parser);
        request.sourceCode = code;
        request.tree = parser.Parse(code);

        SemanticAnalyzer analyzer(nullptr);
        return analyzer.Analyze(request);
    }

    bool HasCode(const std::vector<Diagnostic> &diagnostics, const std::string &code)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&code](const Diagnostic &diag) { return diag.code == code; });
    }
}

// =====================================================================================
// break and continue
// =====================================================================================

TEST_CASE("ControlFlow - Reports break outside a loop or switch")
{
    CHECK(HasCode(AnalyzeFlowSnippet("void Think() { break; }\n"), "as-err-break-outside-loop"));
}

TEST_CASE("ControlFlow - break inside a loop or a switch is accepted")
{
    const std::string code =
        "void Think()\n"
        "{\n"
        "    for (int i = 0; i < 10; i++) { if (i == 3) break; }\n"
        "    while (true) { break; }\n"
        "    switch (1) { case 1: break; }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-break-outside-loop"));
}

TEST_CASE("ControlFlow - Reports continue outside a loop")
{
    CHECK(HasCode(AnalyzeFlowSnippet("void Think() { continue; }\n"), "as-err-continue-outside-loop"));
}

TEST_CASE("ControlFlow - Reports continue in a switch that no loop encloses")
{
    const std::string code =
        "void Think()\n"
        "{\n"
        "    switch (1) { case 1: continue; }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFlowSnippet(code), "as-err-continue-outside-loop"));
}

TEST_CASE("ControlFlow - continue inside a loop is accepted")
{
    const std::string code =
        "void Think()\n"
        "{\n"
        "    for (int i = 0; i < 10; i++) { if (i == 3) continue; }\n"
        "    foreach (int v : g_values) { continue; }\n"
        "}\n"
        "array<int> g_values;\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-continue-outside-loop"));
}

TEST_CASE("ControlFlow - A nested function does not inherit the enclosing loop")
{
    // The lambda's body is its own flow: the `for` around the declaration cannot make a break in it
    // legal.
    const std::string code =
        "funcdef void Callback();\n"
        "void Think()\n"
        "{\n"
        "    for (int i = 0; i < 10; i++)\n"
        "    {\n"
        "        Callback@ cb = function() { break; };\n"
        "    }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFlowSnippet(code), "as-err-break-outside-loop"));
}

// =====================================================================================
// switch
// =====================================================================================

TEST_CASE("ControlFlow - Reports a duplicated case value")
{
    const std::string code =
        "void Think(int mode)\n"
        "{\n"
        "    switch (mode)\n"
        "    {\n"
        "        case 1: break;\n"
        "        case 2: break;\n"
        "        case 1: break;\n"
        "    }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFlowSnippet(code), "as-err-duplicate-case-value"));
}

TEST_CASE("ControlFlow - Distinct case values are accepted")
{
    const std::string code =
        "enum Mode { ModeA, ModeB }\n"
        "void Think(Mode mode)\n"
        "{\n"
        "    switch (mode)\n"
        "    {\n"
        "        case ModeA: break;\n"
        "        case ModeB: break;\n"
        "        default: break;\n"
        "    }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-duplicate-case-value"));
}

TEST_CASE("ControlFlow - Reports a case value that cannot be a label")
{
    const std::string code =
        "void Think(int mode)\n"
        "{\n"
        "    switch (mode)\n"
        "    {\n"
        "        case 'text': break;\n"
        "        case 1.5: break;\n"
        "    }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFlowSnippet(code), "as-err-invalid-case-type"));
}

TEST_CASE("ControlFlow - An enum constant or a named constant is not judged")
{
    const std::string code =
        "const int MAX = 3;\n"
        "void Think(int mode)\n"
        "{\n"
        "    switch (mode)\n"
        "    {\n"
        "        case MAX: break;\n"
        "        case 0x10: break;\n"
        "        case -1: break;\n"
        "    }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-invalid-case-type"));
}

TEST_CASE("ControlFlow - Reports default that is not the last clause")
{
    const std::string code =
        "void Think(int mode)\n"
        "{\n"
        "    switch (mode)\n"
        "    {\n"
        "        default: break;\n"
        "        case 1: break;\n"
        "    }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFlowSnippet(code), "as-err-default-must-be-last"));
}

TEST_CASE("ControlFlow - default as the last clause is accepted")
{
    const std::string code =
        "void Think(int mode)\n"
        "{\n"
        "    switch (mode)\n"
        "    {\n"
        "        case 1: break;\n"
        "        default: break;\n"
        "    }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-default-must-be-last"));
}

// =====================================================================================
// Returning
// =====================================================================================

TEST_CASE("ControlFlow - Reports a non-void function whose body can fall off the end")
{
    CHECK(HasCode(AnalyzeFlowSnippet("int Count() { int x = 1; }\n"), "as-err-not-all-paths-return"));
}

TEST_CASE("ControlFlow - Reports an if with no else as the only return")
{
    const std::string code =
        "int Count(bool flag)\n"
        "{\n"
        "    if (flag) return 1;\n"
        "}\n";

    CHECK(HasCode(AnalyzeFlowSnippet(code), "as-err-not-all-paths-return"));
}

TEST_CASE("ControlFlow - An if/else where both branches return is accepted")
{
    const std::string code =
        "int Count(bool flag)\n"
        "{\n"
        "    if (flag) return 1;\n"
        "    else return 0;\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-not-all-paths-return"));
}

TEST_CASE("ControlFlow - A void function is not judged")
{
    CHECK_FALSE(HasCode(AnalyzeFlowSnippet("void Think() { int x = 1; }\n"),
                        "as-err-not-all-paths-return"));
}

TEST_CASE("ControlFlow - A constructor is not judged")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    Entity() { }\n"
        "    ~Entity() { }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-not-all-paths-return"));
}

TEST_CASE("ControlFlow - A loop with no normal exit ends the function")
{
    const std::string code =
        "int Spin()\n"
        "{\n"
        "    while (true)\n"
        "    {\n"
        "        return 1;\n"
        "    }\n"
        "}\n"
        "int Forever()\n"
        "{\n"
        "    for (;;)\n"
        "    {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-not-all-paths-return"));
}

TEST_CASE("ControlFlow - A switch with a default where every clause returns is accepted")
{
    const std::string code =
        "int Pick(int mode)\n"
        "{\n"
        "    switch (mode)\n"
        "    {\n"
        "        case 1: return 1;\n"
        "        case 2:\n"
        "        case 3: return 2;\n"
        "        default: return 0;\n"
        "    }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-not-all-paths-return"));
}

TEST_CASE("ControlFlow - An interface method has no body to judge")
{
    const std::string code =
        "interface ICounter\n"
        "{\n"
        "    int Count();\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFlowSnippet(code), "as-err-not-all-paths-return"));
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Control Flow Corpus Audit*"`)
// =====================================================================================

TEST_CASE("ControlFlow - Control Flow Corpus Audit" * doctest::skip(true))
{
    static const std::vector<std::string> k_codes = {
        "as-err-break-outside-loop", "as-err-continue-outside-loop", "as-err-invalid-case-type",
        "as-err-duplicate-case-value", "as-err-default-must-be-last", "as-err-not-all-paths-return"
    };

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return std::find(k_codes.begin(), k_codes.end(), code) != k_codes.end();
    });

    MESSAGE("Control-flow corpus audit: files=" << result.filesAnalysed
            << " totalFlagged=" << result.Total()
            << " seconds=" << result.seconds);

    for (const auto &[code, count] : result.countByCode)
    {
        MESSAGE("  " << code << ": " << count);
    }
    for (const auto &hit : result.hits)
    {
        MESSAGE("  " << hit.fileName << ":" << hit.line << " [" << hit.code << "] " << hit.message);
    }

    CHECK(result.filesAnalysed > 0);

    // One finding survives triage and it is genuine: AngelScript_SC_Plugins_ChatTriggers.as
    // declares `HookReturnCode PlayerSay(SayParameters@ pParams)` with a body that reads one
    // argument and then ends - an unfinished hook that would leave the caller with no return value.
    CHECK(result.Total() == 1);
    REQUIRE(result.hits.size() == 1);
    CHECK(result.hits[0].code == "as-err-not-all-paths-return");
    CHECK(result.hits[0].fileName == "AngelScript_SC_Plugins_ChatTriggers.as");
}
