#include <doctest/doctest.h>

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "config/EngineRuleParser.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
std::vector<Diagnostic> AnalyzeWithRules(const std::string& code,
                                         const angel_lsp::config::EngineRuleConfig* rules = nullptr,
                                         const std::string& fileUri = "file:///engine_rules_test.as")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    collector.CollectSymbols(fileUri, code, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    request.tree = parser.Parse(code);
    request.engineRules = rules;

    SemanticAnalyzer analyzer(nullptr);
    auto diagnostics = analyzer.Analyze(request);

    if (request.tree)
    {
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    }
    return diagnostics;
}

bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [code](const Diagnostic& diag) { return diag.code == code; });
}
} // namespace

TEST_CASE("EngineRuleParser - Parses valid rules JSON and properties")
{
    const std::string jsonText = R"({
        "name": "Test Rules",
        "engineProperties": {
            "propertyAccessorMode": 2,
            "disallowGlobalVars": true
        },
        "storageRules": [
            {
                "id": "unsafe-handle",
                "inheritsFrom": "CBaseEntity",
                "typeRegex": "CBase.*",
                "handleOnly": true,
                "disallowInMembers": true,
                "disallowInArrays": true,
                "suggestReplacement": "EHandle"
            }
        ],
        "typeSuggestions": [
            {
                "id": "prefer-regular-string",
                "fromType": "string_t",
                "toType": "string",
                "appliesTo": ["local", "member"]
            }
        ],
        "schedulerRules": [
            {
                "id": "sched-safety",
                "callee": "g_Scheduler",
                "methods": ["SetTimeout"],
                "disallowArgRegex": "CBase.*",
                "disallowArgHandleOnly": true,
                "argSuggestReplacement": "EHandle",
                "requireExplicitEnumConstruct": true
            }
        ]
    })";

    auto parsed = angel_lsp::config::ParseEngineRuleConfigJson(jsonText);
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == "Test Rules");
    CHECK(parsed->engineProperties.propertyAccessorMode == 2);
    CHECK(parsed->engineProperties.disallowGlobalVars == true);
    CHECK(parsed->storageRules.size() == 1);
    CHECK(parsed->storageRules[0].id == "unsafe-handle");
    CHECK(parsed->typeSuggestions.size() == 1);
    CHECK(parsed->typeSuggestions[0].fromType == "string_t");
    CHECK(parsed->schedulerRules.size() == 1);
    CHECK(parsed->schedulerRules[0].id == "sched-safety");
}

TEST_CASE("EngineRuleParser - Handles invalid JSON gracefully")
{
    CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson("invalid json {").has_value());
    CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson("12345").has_value());
    CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson("[]").has_value());
}

TEST_CASE("EngineRuleChecker - Pure Vanilla AngelScript has zero warnings by default")
{
    std::mt19937_64 rng(0x42A110);
    const std::string className = angel_lsp::test::GenerateIdentifier(rng, "CBaseMonster");
    const std::string varName = angel_lsp::test::GenerateIdentifier(rng, "m_target");

    const std::string code = "class " + className +
                             " {}\n"
                             "class Container {\n"
                             "    " +
                             className + "@ " + varName +
                             ";\n"
                             "}\n";

    auto diags = AnalyzeWithRules(code, nullptr);
    CHECK_FALSE(HasCode(diags, angel_lsp::diagnostics::codes::EngineStorageRule));
    CHECK_FALSE(HasCode(diags, angel_lsp::diagnostics::codes::EngineTypeSuggestion));
}

TEST_CASE("EngineRuleChecker - Enforces storage constraints on member and array handles")
{
    std::mt19937_64 rng(0x42A111);
    const std::string entityType = angel_lsp::test::GenerateIdentifier(rng, "CBaseEntity");
    const std::string memberVar = angel_lsp::test::GenerateIdentifier(rng, "m_ent");
    const std::string arrayVar = angel_lsp::test::GenerateIdentifier(rng, "m_arr");
    const std::string localVar = angel_lsp::test::GenerateIdentifier(rng, "localEnt");

    angel_lsp::config::EngineRuleConfig config;
    angel_lsp::config::StorageRule rule;
    rule.id = "unsafe-entity-handle";
    rule.typeRegex = "CBase.*";
    rule.handleOnly = true;
    rule.disallowInMembers = true;
    rule.disallowInArrays = true;
    rule.suggestReplacement = "EHandle";
    config.storageRules.push_back(rule);

    const std::string code = "class " + entityType +
                             " {}\n"
                             "class SafeHolder {\n"
                             "    " +
                             entityType + "@ " + memberVar +
                             ";\n"
                             "    array<" +
                             entityType + "@> " + arrayVar +
                             ";\n"
                             "    void DoWork() {\n"
                             "        " +
                             entityType + "@ " + localVar +
                             ";\n"
                             "    }\n"
                             "}\n";

    auto diags = AnalyzeWithRules(code, &config);
    CHECK(HasCode(diags, angel_lsp::diagnostics::codes::EngineStorageRule));
}

TEST_CASE("EngineRuleChecker - Enforces local-only storage rules")
{
    std::mt19937_64 rng(0x42A112);
    const std::string typeName = angel_lsp::test::GenerateIdentifier(rng, "CustomKeyvalue");
    const std::string globalVar = angel_lsp::test::GenerateIdentifier(rng, "g_kv");
    const std::string memberVar = angel_lsp::test::GenerateIdentifier(rng, "m_kv");
    const std::string localVar = angel_lsp::test::GenerateIdentifier(rng, "kv");

    angel_lsp::config::EngineRuleConfig config;
    angel_lsp::config::StorageRule rule;
    rule.id = "local-only-storage";
    rule.types = {typeName};
    rule.disallowInMembers = true;
    rule.disallowInGlobals = true;
    config.storageRules.push_back(rule);

    const std::string code = "class " + typeName + " {}\n" + typeName + " " + globalVar +
                             ";\n"
                             "class Holder {\n"
                             "    " +
                             typeName + " " + memberVar +
                             ";\n"
                             "    void Test() {\n"
                             "        " +
                             typeName + " " + localVar +
                             ";\n"
                             "    }\n"
                             "}\n";

    auto diags = AnalyzeWithRules(code, &config);
    CHECK(HasCode(diags, angel_lsp::diagnostics::codes::EngineStorageRule));
}

TEST_CASE("EngineRuleChecker - Suggests type replacement when configured")
{
    std::mt19937_64 rng(0x42A113);
    const std::string varName = angel_lsp::test::GenerateIdentifier(rng, "my_str");

    angel_lsp::config::EngineRuleConfig config;
    angel_lsp::config::TypeSuggestionRule rule;
    rule.id = "prefer-regular-string";
    rule.fromType = "string_ts";
    rule.toType = "string";
    rule.appliesTo = {"local", "member"};
    config.typeSuggestions.push_back(rule);

    const std::string code = "class string_ts {}\n"
                             "class string {}\n"
                             "void Func() {\n"
                             "    string_ts " +
                             varName +
                             ";\n"
                             "}\n";

    auto diags = AnalyzeWithRules(code, &config);
    CHECK(HasCode(diags, angel_lsp::diagnostics::codes::EngineTypeSuggestion));
}

TEST_CASE("EngineRuleParser - Loads predefined sven rules file from repo")
{
    const std::filesystem::path rulesFile =
        std::filesystem::path(ANGELSCRIPT_REPO_ROOT) / "predefined" / "sven.angelscript.rules.json";
    REQUIRE(std::filesystem::exists(rulesFile));

    auto parsed = angel_lsp::config::ParseEngineRuleConfigFile(rulesFile);
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == "Sven Co-op Engine Safety Rules");
    CHECK(parsed->storageRules.size() >= 2);
    CHECK(parsed->typeSuggestions.size() >= 1);
    CHECK(parsed->schedulerRules.size() >= 1);
}

TEST_CASE("EngineRuleChecker - Enforces scheduler argument safety")
{
    std::mt19937_64 rng(0x42A114);
    const std::string entType = angel_lsp::test::GenerateIdentifier(rng, "CBaseEntity");
    const std::string entVar = angel_lsp::test::GenerateIdentifier(rng, "ent");

    angel_lsp::config::EngineRuleConfig config;
    angel_lsp::config::SchedulerRule rule;
    rule.id = "scheduler-safety";
    rule.receiverTypes = {"g_Scheduler"};
    rule.methodNames = {"SetTimeout"};
    rule.disallowArgRegex = "CBase.*";
    config.schedulerRules.push_back(rule);

    const std::string code = "class " + entType +
                             " {}\n"
                             "class Scheduler {\n"
                             "    void SetTimeout(string fn, float delay, " +
                             entType +
                             "@ e) {}\n"
                             "}\n"
                             "Scheduler g_Scheduler;\n"
                             "void Main() {\n"
                             "    " +
                             entType + "@ " + entVar +
                             ";\n"
                             "    g_Scheduler.SetTimeout(\"cb\", 1.0f, " +
                             entVar +
                             ");\n"
                             "}\n";

    auto diags = AnalyzeWithRules(code, &config);
    CHECK(HasCode(diags, angel_lsp::diagnostics::codes::EngineSchedulerSafety));
}

TEST_CASE("EngineRuleChecker - Enforces explicit enum construct on scheduled discriminant")
{
    std::mt19937_64 rng(0x42A115);
    const std::string enumType = angel_lsp::test::GenerateIdentifier(rng, "ActionType");

    angel_lsp::config::EngineRuleConfig config;
    angel_lsp::config::SchedulerRule rule;
    rule.id = "scheduler-safety";
    rule.receiverTypes = {"g_Scheduler"};
    rule.methodNames = {"SetTimeout"};
    rule.requireExplicitEnumConstruct = true;
    config.schedulerRules.push_back(rule);

    const std::string code = "enum " + enumType +
                             " { Alpha = 1, Beta = 2 }\n"
                             "class Scheduler {\n"
                             "    void SetTimeout(string fn, float delay, " +
                             enumType +
                             " act) {}\n"
                             "}\n"
                             "Scheduler g_Scheduler;\n"
                             "void Main() {\n"
                             "    g_Scheduler.SetTimeout(\"cb\", 1.0f, 1);\n"
                             "}\n";

    auto diags = AnalyzeWithRules(code, &config);
    CHECK(HasCode(diags, angel_lsp::diagnostics::codes::EngineScheduledEnumDiscriminant));
}
