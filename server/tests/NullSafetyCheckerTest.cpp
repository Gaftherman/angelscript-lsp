#include <doctest/doctest.h>

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/NullSafetyChecker.h"
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
                                      const config::DiagnosticsConfig* diagConfig = nullptr,
                                      const std::string& fileUri = "file:///null_safety_test.as")
{
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

bool HasNullDereferenceWarning(const std::vector<Diagnostic>& diagnostics, const std::string& expectedSymbol = "")
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d)
                       {
                           if (d.code != diagnostics::codes::PossibleNullDereference)
                           {
                               return false;
                           }
                           if (!expectedSymbol.empty())
                           {
                               return d.message.find(expectedSymbol) != std::string::npos;
                           }
                           return true;
                       });
}
} // namespace

TEST_SUITE("NullSafetyChecker")
{
    TEST_CASE("Unguarded Handle Parameter Dereference")
    {
        const std::string typeName = GenerateRandomSymbolName("PlayerType");
        const std::string paramName = GenerateRandomSymbolName("pPlayer");
        const std::string methodName = GenerateRandomSymbolName("ResetModel");
        const std::string funcName = GenerateRandomSymbolName("OnThink");

        const std::string code = "class " + typeName + " { void " + methodName + "() {} }\n" +
                                 "void " + funcName + "(" + typeName + "@ " + paramName + ")\n" +
                                 "{\n" +
                                 "    " + paramName + "." + methodName + "();\n" +
                                 "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK(HasNullDereferenceWarning(diags, paramName));
    }

    TEST_CASE("Guarded Handle Parameter With If Not Null")
    {
        const std::string typeName = GenerateRandomSymbolName("PlayerType");
        const std::string paramName = GenerateRandomSymbolName("pPlayer");
        const std::string methodName = GenerateRandomSymbolName("ResetModel");
        const std::string funcName = GenerateRandomSymbolName("OnThink");

        const std::string code = "class " + typeName + " { void " + methodName + "() {} }\n" +
                                 "void " + funcName + "(" + typeName + "@ " + paramName + ")\n" +
                                 "{\n" +
                                 "    if (" + paramName + " !is null)\n" +
                                 "    {\n" +
                                 "        " + paramName + "." + methodName + "();\n" +
                                 "    }\n" +
                                 "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags, paramName));
    }

    TEST_CASE("Guarded Handle Parameter With Early Guard Return")
    {
        const std::string typeName = GenerateRandomSymbolName("PlayerType");
        const std::string paramName = GenerateRandomSymbolName("pPlayer");
        const std::string methodName = GenerateRandomSymbolName("ResetModel");
        const std::string funcName = GenerateRandomSymbolName("OnThink");

        const std::string code = "class " + typeName + " { void " + methodName + "() {} }\n" +
                                 "void " + funcName + "(" + typeName + "@ " + paramName + ")\n" +
                                 "{\n" +
                                 "    if (" + paramName + " is null)\n" +
                                 "        return;\n" +
                                 "    " + paramName + "." + methodName + "();\n" +
                                 "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags, paramName));
    }

    TEST_CASE("Short Circuit Conjunction Guard")
    {
        const std::string typeName = GenerateRandomSymbolName("PlayerType");
        const std::string paramName = GenerateRandomSymbolName("pPlayer");
        const std::string isAliveMethod = GenerateRandomSymbolName("IsAlive");
        const std::string methodName = GenerateRandomSymbolName("ResetModel");
        const std::string funcName = GenerateRandomSymbolName("OnThink");

        const std::string code = "class " + typeName + " { bool " + isAliveMethod + "() { return true; } void " + methodName + "() {} }\n" +
                                 "void " + funcName + "(" + typeName + "@ " + paramName + ")\n" +
                                 "{\n" +
                                 "    if (" + paramName + " !is null && " + paramName + "." + isAliveMethod + "())\n" +
                                 "    {\n" +
                                 "        " + paramName + "." + methodName + "();\n" +
                                 "    }\n" +
                                 "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags, paramName));
    }

    TEST_CASE("Lambda Expression Parameter Dereference Unguarded vs Guarded")
    {
        const std::string typeName = GenerateRandomSymbolName("CBasePlayer");
        const std::string paramName = GenerateRandomSymbolName("player");
        const std::string methodName = GenerateRandomSymbolName("ResetOverriddenPlayerModel");
        const std::string regFunc = GenerateRandomSymbolName("RegisterHook");

        const std::string unguardedCode =
            "class " + typeName + " { void " + methodName + "(bool a, bool b) {} }\n" +
            "funcdef void HookCb(" + typeName + "@);\n" +
            "void " + regFunc + "(HookCb@ cb) {}\n" +
            "void Init()\n" +
            "{\n" +
            "    " + regFunc + "(function(" + typeName + "@ " + paramName + ") {\n" +
            "        " + paramName + "." + methodName + "(true, true);\n" +
            "    });\n" +
            "}\n";

        const auto unguardedDiags = AnalyzeScript(unguardedCode);
        CHECK(HasNullDereferenceWarning(unguardedDiags, paramName));

        const std::string guardedCode =
            "class " + typeName + " { void " + methodName + "(bool a, bool b) {} }\n" +
            "funcdef void HookCb(" + typeName + "@);\n" +
            "void " + regFunc + "(HookCb@ cb) {}\n" +
            "void Init()\n" +
            "{\n" +
            "    " + regFunc + "(function(" + typeName + "@ " + paramName + ") {\n" +
            "        if (" + paramName + " !is null) {\n" +
            "            " + paramName + "." + methodName + "(true, true);\n" +
            "        }\n" +
            "    });\n" +
            "}\n";

        const auto guardedDiags = AnalyzeScript(guardedCode);
        CHECK_FALSE(HasNullDereferenceWarning(guardedDiags, paramName));
    }

    TEST_CASE("Cast Expression Dereference Unguarded vs Guarded")
    {
        const std::string baseType = GenerateRandomSymbolName("BaseEntity");
        const std::string derivedType = GenerateRandomSymbolName("PlayerEntity");
        const std::string entityParam = GenerateRandomSymbolName("pEntity");
        const std::string playerVar = GenerateRandomSymbolName("pPlayer");
        const std::string methodName = GenerateRandomSymbolName("Action");

        const std::string unguardedCode =
            "class " + baseType + " {}\n" +
            "class " + derivedType + " : " + baseType + " { void " + methodName + "() {} }\n" +
            "void Test(" + baseType + "@ " + entityParam + ")\n" +
            "{\n" +
            "    " + derivedType + "@ " + playerVar + " = cast<" + derivedType + "@>(" + entityParam + ");\n" +
            "    " + playerVar + "." + methodName + "();\n" +
            "}\n";

        const auto unguardedDiags = AnalyzeScript(unguardedCode);
        CHECK(HasNullDereferenceWarning(unguardedDiags, playerVar));

        const std::string directCastCode =
            "class " + baseType + " {}\n" +
            "class " + derivedType + " : " + baseType + " { void " + methodName + "() {} }\n" +
            "void Test(" + baseType + "@ " + entityParam + ")\n" +
            "{\n" +
            "    cast<" + derivedType + "@>(" + entityParam + ")." + methodName + "();\n" +
            "}\n";

        const auto directDiags = AnalyzeScript(directCastCode);
        CHECK(HasNullDereferenceWarning(directDiags));

        const std::string guardedCode =
            "class " + baseType + " {}\n" +
            "class " + derivedType + " : " + baseType + " { void " + methodName + "() {} }\n" +
            "void Test(" + baseType + "@ " + entityParam + ")\n" +
            "{\n" +
            "    " + derivedType + "@ " + playerVar + " = cast<" + derivedType + "@>(" + entityParam + ");\n" +
            "    if (" + playerVar + " !is null)\n" +
            "    {\n" +
            "        " + playerVar + "." + methodName + "();\n" +
            "    }\n" +
            "}\n";

        const auto guardedDiags = AnalyzeScript(guardedCode);
        CHECK_FALSE(HasNullDereferenceWarning(guardedDiags, playerVar));
    }

    TEST_CASE("Multiple Parameters Selective Guard")
    {
        const std::string victimType = GenerateRandomSymbolName("VictimType");
        const std::string attackerType = GenerateRandomSymbolName("AttackerType");
        const std::string victimParam = GenerateRandomSymbolName("pVictim");
        const std::string attackerParam = GenerateRandomSymbolName("pAttacker");

        const std::string code =
            "class " + victimType + " { void OnKilled() {} }\n" +
            "class " + attackerType + " { void TakeReward() {} }\n" +
            "void OnEvent(" + victimType + "@ " + victimParam + ", " + attackerType + "@ " + attackerParam + ")\n" +
            "{\n" +
            "    if (" + victimParam + " !is null)\n" +
            "    {\n" +
            "        " + victimParam + ".OnKilled();\n" +
            "    }\n" +
            "    " + attackerParam + ".TakeReward();\n" +
            "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags, victimParam));
        CHECK(HasNullDereferenceWarning(diags, attackerParam));
    }

    TEST_CASE("Value Types and Non-Handle Types are Ignored")
    {
        const std::string valName = GenerateRandomSymbolName("vec");
        const std::string intName = GenerateRandomSymbolName("counter");

        const std::string code =
            std::string("class Vector { void Normalize() {} }\n") +
            "void Process(Vector " + valName + ", int " + intName + ")\n" +
            "{\n" +
            "    " + valName + ".Normalize();\n" +
            "    int y = " + intName + " + 1;\n" +
            "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags));
    }

    TEST_CASE("Configuration Flag Can Suppress Null Dereference Diagnostics")
    {
        const std::string typeName = GenerateRandomSymbolName("TargetType");
        const std::string paramName = GenerateRandomSymbolName("pTarget");

        const std::string code =
            "class " + typeName + " { void DoAction() {} }\n" +
            "void Exec(" + typeName + "@ " + paramName + ")\n" +
            "{\n" +
            "    " + paramName + ".DoAction();\n" +
            "}\n";

        config::DiagnosticsConfig disabledConfig;
        disabledConfig.reportPossibleNullDereference = false;

        const auto diags = AnalyzeScript(code, &disabledConfig);
        CHECK_FALSE(HasNullDereferenceWarning(diags, paramName));
    }

    TEST_CASE("Sven Co-op Hook Lambda Pattern Dereference")
    {
        const std::string playerType = GenerateRandomSymbolName("CBasePlayer");
        const std::string playerParam = GenerateRandomSymbolName("player");
        const std::string resetMethod = GenerateRandomSymbolName("ResetOverriddenPlayerModel");
        const std::string edictMethod = GenerateRandomSymbolName("edict");

        const std::string guardedCode =
            "class " + playerType + " {\n" +
            "    void " + resetMethod + "(bool a, bool b) {}\n" +
            "    int " + edictMethod + "() { return 0; }\n" +
            "}\n" +
            "funcdef void PlayerPostThinkHook(" + playerType + "@);\n" +
            "void RegisterHook(int hook, PlayerPostThinkHook@ cb) {}\n" +
            "void MapActivate()\n" +
            "{\n" +
            "    RegisterHook(1, @PlayerPostThinkHook(function(" + playerType + "@ " + playerParam + ") {\n" +
            "        if (" + playerParam + " !is null) {\n" +
            "            " + playerParam + "." + resetMethod + "(true, true);\n" +
            "            int e = " + playerParam + "." + edictMethod + "();\n" +
            "        }\n" +
            "    }));\n" +
            "}\n";

        const auto guardedDiags = AnalyzeScript(guardedCode);
        CHECK_FALSE(HasNullDereferenceWarning(guardedDiags, playerParam));

        const std::string unguardedCode =
            "class " + playerType + " {\n" +
            "    void " + resetMethod + "(bool a, bool b) {}\n" +
            "    int " + edictMethod + "() { return 0; }\n" +
            "}\n" +
            "funcdef void PlayerPostThinkHook(" + playerType + "@);\n" +
            "void RegisterHook(int hook, PlayerPostThinkHook@ cb) {}\n" +
            "void MapActivate()\n" +
            "{\n" +
            "    RegisterHook(1, @PlayerPostThinkHook(function(" + playerType + "@ " + playerParam + ") {\n" +
            "        " + playerParam + "." + resetMethod + "(true, true);\n" +
            "    }));\n" +
            "}\n";

        const auto unguardedDiags = AnalyzeScript(unguardedCode);
        CHECK(HasNullDereferenceWarning(unguardedDiags, playerParam));
    }
}
} // namespace angel_lsp::test
