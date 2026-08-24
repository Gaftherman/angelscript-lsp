#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "analysis/rules/VariableRules.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "config/ServerConfig.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    std::vector<Diagnostic> AnalyzeVariableSnippet(const std::string &code,
                                                   const std::string &fileUri = "file:///vars.as",
                                                   const angel_lsp::config::EngineProperties *engine = nullptr)
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static angel_lsp::i18n::I18n i18n;

        collector.CollectSymbols(fileUri, code, parser, table);

        SemanticAnalysisRequest request{ table, fileUri, ".as.predefined", &i18n };
        request.engineProperties = engine;
        request.scopeRoot = scopes.CollectScopes(code, parser);
        request.sourceCode = code;

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
// Declared type
// =====================================================================================

TEST_CASE("VariableRules - Reports a variable declared void")
{
    CHECK(HasCode(AnalyzeVariableSnippet("void g_nothing;\n"), "as-err-void-variable"));
}

TEST_CASE("VariableRules - Reports a handle on a primitive")
{
    CHECK(HasCode(AnalyzeVariableSnippet("int@ g_broken;\n"), "as-err-handle-on-primitive"));
}

TEST_CASE("VariableRules - Reports a funcdef declared without a handle")
{
    const std::string code =
        "funcdef void Callback();\n"
        "Callback g_handler;\n";

    CHECK(HasCode(AnalyzeVariableSnippet(code), "as-err-funcdef-not-handle"));
}

TEST_CASE("VariableRules - A funcdef handle is accepted")
{
    const std::string code =
        "funcdef void Callback();\n"
        "Callback@ g_handler;\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code), "as-err-funcdef-not-handle"));
}

TEST_CASE("VariableRules - Reports a mixin used as a data type")
{
    const std::string code =
        "mixin class Helper {}\n"
        "Helper g_helper;\n";

    CHECK(HasCode(AnalyzeVariableSnippet(code), "as-err-mixin-not-a-type"));
}

TEST_CASE("VariableRules - An array of handles is not a double handle")
{
    // Regression, and a collector bug rather than a rule one: TypeExtraction folded the template
    // argument's handle into the outer type, so the '@' of `array<Foo@>@` read as a second handle
    // and every such declaration was reported as a handle on a primitive. It is ordinary
    // AngelScript and appears throughout the corpus.
    const std::string code =
        "class Schedule {}\n"
        "array<Schedule@>@ g_schedules;\n"
        "array<Schedule@> g_owned;\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code), "as-err-handle-on-primitive"));
}

TEST_CASE("VariableRules - A type this analyzer cannot see is never judged")
{
    const auto diagnostics = AnalyzeVariableSnippet("CBaseEntity g_entity;\nCScheduler@ g_sched;\n");
    CHECK_FALSE(HasCode(diagnostics, "as-err-funcdef-not-handle"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-mixin-not-a-type"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-void-variable"));
}

// =====================================================================================
// Placement
// =====================================================================================

TEST_CASE("VariableRules - Reports an access modifier on a global")
{
    CHECK(HasCode(AnalyzeVariableSnippet("private int g_count;\n"),
                  "as-err-global-variable-access-modifier"));
}

TEST_CASE("VariableRules - An access modifier on a class field is accepted")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    private int m_health;\n"
        "    protected string m_name;\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code), "as-err-global-variable-access-modifier"));
}

TEST_CASE("VariableRules - Reports a const class member")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    const int m_max = 10;\n"
        "}\n";

    CHECK(HasCode(AnalyzeVariableSnippet(code), "as-err-class-member-const"));
}

TEST_CASE("VariableRules - A const at namespace or global scope is accepted")
{
    const std::string code =
        "const int MAX = 10;\n"
        "namespace Weapon\n"
        "{\n"
        "    const string MODEL = \"axe.mdl\";\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code), "as-err-class-member-const"));
}

// =====================================================================================
// Virtual properties
// =====================================================================================

TEST_CASE("VariableRules - Reports a virtual property accessor with no body")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    string Name\n"
        "    {\n"
        "        get;\n"
        "    }\n"
        "}\n";

    CHECK(HasCode(AnalyzeVariableSnippet(code), "as-err-property-accessor-missing-body"));
}

TEST_CASE("VariableRules - An interface property needs no accessor body")
{
    const std::string code =
        "interface INamed\n"
        "{\n"
        "    string Name { get; set; }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code), "as-err-property-accessor-missing-body"));
}

TEST_CASE("VariableRules - A property with bodies is accepted")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    private string m_name;\n"
        "    string Name\n"
        "    {\n"
        "        get const { return m_name; }\n"
        "        set { m_name = value; }\n"
        "    }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code), "as-err-property-accessor-missing-body"));
}

TEST_CASE("VariableRules - Reports a duplicated property accessor")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    private string m_name;\n"
        "    string Name\n"
        "    {\n"
        "        get const { return m_name; }\n"
        "        get const { return m_name; }\n"
        "    }\n"
        "}\n";

    CHECK(HasCode(AnalyzeVariableSnippet(code), "as-err-property-duplicate-accessor"));
}

TEST_CASE("VariableRules - One get and one set are not duplicates")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    private string m_name;\n"
        "    string Name\n"
        "    {\n"
        "        get const { return m_name; }\n"
        "        set { m_name = value; }\n"
        "    }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code), "as-err-property-duplicate-accessor"));
}

TEST_CASE("VariableRules - Reports a virtual property on a mixin class")
{
    const std::string code =
        "mixin class Helper\n"
        "{\n"
        "    string Name { get { return ''; } }\n"
        "}\n";

    CHECK(HasCode(AnalyzeVariableSnippet(code), "as-err-mixin-virtual-property"));
}

TEST_CASE("VariableRules - A predefined stub is exempt")
{
    const auto diagnostics = AnalyzeVariableSnippet("private int g_count;\nint@ g_handle;\n",
                                                    "file:///engine.as.predefined");
    CHECK_FALSE(HasCode(diagnostics, "as-err-global-variable-access-modifier"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-handle-on-primitive"));
}

// =====================================================================================
// asEP_DISALLOW_GLOBAL_VARS
// =====================================================================================

TEST_CASE("VariableRules - Reports a global variable when the host disallows them")
{
    // The engine's own wording: "Global variables have been disabled by the application".
    angel_lsp::config::EngineProperties engine;
    engine.disallowGlobalVars = true;

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet("int g_count;\n"), "as-err-global-vars-disallowed"));
    CHECK(HasCode(AnalyzeVariableSnippet("int g_count;\n", "file:///vars.as", &engine),
                  "as-err-global-vars-disallowed"));
}

TEST_CASE("VariableRules - A class member is not a global variable")
{
    angel_lsp::config::EngineProperties engine;
    engine.disallowGlobalVars = true;

    const std::string code =
        "class Entity\n"
        "{\n"
        "    int health;\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code, "file:///vars.as", &engine),
                        "as-err-global-vars-disallowed"));
}

TEST_CASE("VariableRules - A local is not a global variable")
{
    angel_lsp::config::EngineProperties engine;
    engine.disallowGlobalVars = true;

    const std::string code =
        "void Think()\n"
        "{\n"
        "    int ticks = 0;\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code, "file:///vars.as", &engine),
                        "as-err-global-vars-disallowed"));
}

// =====================================================================================
// Template arguments
// =====================================================================================

TEST_CASE("VariableRules - Reports a template instantiated with void")
{
    // The engine's own wording: "Attempting to instantiate invalid template 'array<void>'".
    CHECK(HasCode(AnalyzeVariableSnippet("array<void> g_bad;\n"), "as-err-array-invalid-template"));
}

TEST_CASE("VariableRules - Ordinary template arguments are not judged")
{
    // Everything else is either valid or decided by a host registration this analyzer cannot see -
    // `array<CBasePlayer@>` is most of the corpus, and reporting it would be reporting the engine.
    const std::string code =
        "class Entity {}\n"
        "array<int> g_numbers;\n"
        "array<Entity> g_entities;\n"
        "array<Entity@> g_handles;\n"
        "array<CBaseUnknown@> g_engineTypes;\n"
        "array<array<int>> g_nested;\n";

    CHECK_FALSE(HasCode(AnalyzeVariableSnippet(code), "as-err-array-invalid-template"));
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Variable Rules Corpus Audit*"`)
// =====================================================================================

TEST_CASE("VariableRules - Variable Rules Corpus Audit" * doctest::skip(true))
{
    static const std::vector<std::string> k_codes = {
        "as-err-void-variable", "as-err-handle-on-primitive", "as-err-funcdef-not-handle",
        "as-err-mixin-not-a-type", "as-err-global-variable-access-modifier",
        "as-err-property-accessor-missing-body", "as-err-mixin-virtual-property",
        "as-err-property-duplicate-accessor",
        "as-err-class-member-const", "as-err-array-invalid-template"
    };

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return std::find(k_codes.begin(), k_codes.end(), code) != k_codes.end();
    });

    MESSAGE("Variable-rule corpus audit: files=" << result.filesAnalysed
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

    // One finding survives triage, on AngelScript-SvenCoop_oldone.as - a file that is not
    // AngelScript at all but a C++ header pasted into a .as (`public:` labels, `entvars_t
    // *pevAttacker`). The parser reports it too; the void declaration is what error recovery makes
    // of the malformed line. Everything else the rules reported over the corpus was a false
    // positive and was fixed at its source.
    CHECK(result.Total() == 1);
    REQUIRE(result.hits.size() == 1);
    CHECK(result.hits[0].code == "as-err-void-variable");
}
