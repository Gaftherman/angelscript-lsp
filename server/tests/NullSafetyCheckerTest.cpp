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

TEST_SUITE_BEGIN("NullSafetyChecker");

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
        const std::string hookDef = GenerateRandomSymbolName("HookCb");
        const std::string initFunc = GenerateRandomSymbolName("Init");

        const std::string unguardedCode =
            "class " + typeName + " { void " + methodName + "(bool a, bool b) {} }\n" +
            "funcdef void " + hookDef + "(" + typeName + "@);\n" +
            "void " + regFunc + "(" + hookDef + "@ cb) {}\n" +
            "void " + initFunc + "()\n" +
            "{\n" +
            "    " + regFunc + "(function(" + typeName + "@ " + paramName + ") {\n" +
            "        " + paramName + "." + methodName + "(true, true);\n" +
            "    });\n" +
            "}\n";

        const auto unguardedDiags = AnalyzeScript(unguardedCode);
        CHECK(HasNullDereferenceWarning(unguardedDiags, paramName));

        const std::string guardedCode =
            "class " + typeName + " { void " + methodName + "(bool a, bool b) {} }\n" +
            "funcdef void " + hookDef + "(" + typeName + "@);\n" +
            "void " + regFunc + "(" + hookDef + "@ cb) {}\n" +
            "void " + initFunc + "()\n" +
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
        const std::string testFunc = GenerateRandomSymbolName("TestFunc");

        const std::string unguardedCode =
            "class " + baseType + " {}\n" +
            "class " + derivedType + " : " + baseType + " { void " + methodName + "() {} }\n" +
            "void " + testFunc + "(" + baseType + "@ " + entityParam + ")\n" +
            "{\n" +
            "    " + derivedType + "@ " + playerVar + " = cast<" + derivedType + "@>(" + entityParam + ");\n" +
            "    " + playerVar + "." + methodName + "();\n" +
            "}\n";

        const auto unguardedDiags = AnalyzeScript(unguardedCode);
        CHECK(HasNullDereferenceWarning(unguardedDiags, playerVar));

        const std::string directCastCode =
            "class " + baseType + " {}\n" +
            "class " + derivedType + " : " + baseType + " { void " + methodName + "() {} }\n" +
            "void " + testFunc + "(" + baseType + "@ " + entityParam + ")\n" +
            "{\n" +
            "    cast<" + derivedType + "@>(" + entityParam + ")." + methodName + "();\n" +
            "}\n";

        const auto directDiags = AnalyzeScript(directCastCode);
        CHECK(HasNullDereferenceWarning(directDiags));

        const std::string guardedCode =
            "class " + baseType + " {}\n" +
            "class " + derivedType + " : " + baseType + " { void " + methodName + "() {} }\n" +
            "void " + testFunc + "(" + baseType + "@ " + entityParam + ")\n" +
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
        const std::string onKilledMethod = GenerateRandomSymbolName("OnKilled");
        const std::string takeRewardMethod = GenerateRandomSymbolName("TakeReward");
        const std::string onEventFunc = GenerateRandomSymbolName("OnEvent");

        const std::string code =
            "class " + victimType + " { void " + onKilledMethod + "() {} }\n" +
            "class " + attackerType + " { void " + takeRewardMethod + "() {} }\n" +
            "void " + onEventFunc + "(" + victimType + "@ " + victimParam + ", " + attackerType + "@ " + attackerParam + ")\n" +
            "{\n" +
            "    if (" + victimParam + " !is null)\n" +
            "    {\n" +
            "        " + victimParam + "." + onKilledMethod + "();\n" +
            "    }\n" +
            "    " + attackerParam + "." + takeRewardMethod + "();\n" +
            "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags, victimParam));
        CHECK(HasNullDereferenceWarning(diags, attackerParam));
    }

    TEST_CASE("Value Types and Non-Handle Types are Ignored")
    {
        const std::string valType = GenerateRandomSymbolName("Vector");
        const std::string valMethod = GenerateRandomSymbolName("Normalize");
        const std::string procFunc = GenerateRandomSymbolName("Process");
        const std::string valName = GenerateRandomSymbolName("vec");
        const std::string intName = GenerateRandomSymbolName("counter");

        const std::string code =
            "class " + valType + " { void " + valMethod + "() {} }\n" +
            "void " + procFunc + "(" + valType + " " + valName + ", int " + intName + ")\n" +
            "{\n" +
            "    " + valName + "." + valMethod + "();\n" +
            "    int y = " + intName + " + 1;\n" +
            "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags));
    }

    TEST_CASE("Configuration Flag Can Suppress Null Dereference Diagnostics")
    {
        const std::string typeName = GenerateRandomSymbolName("TargetType");
        const std::string paramName = GenerateRandomSymbolName("pTarget");
        const std::string doActionMethod = GenerateRandomSymbolName("DoAction");
        const std::string execFunc = GenerateRandomSymbolName("Exec");

        const std::string code =
            "class " + typeName + " { void " + doActionMethod + "() {} }\n" +
            "void " + execFunc + "(" + typeName + "@ " + paramName + ")\n" +
            "{\n" +
            "    " + paramName + "." + doActionMethod + "();\n" +
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
        const std::string hookDef = GenerateRandomSymbolName("PlayerPostThinkHook");
        const std::string regFunc = GenerateRandomSymbolName("RegisterHook");
        const std::string mapActFunc = GenerateRandomSymbolName("MapActivate");

        const std::string guardedCode =
            "class " + playerType + " {\n" +
            "    void " + resetMethod + "(bool a, bool b) {}\n" +
            "    int " + edictMethod + "() { return 0; }\n" +
            "}\n" +
            "funcdef void " + hookDef + "(" + playerType + "@);\n" +
            "void " + regFunc + "(int hook, " + hookDef + "@ cb) {}\n" +
            "void " + mapActFunc + "()\n" +
            "{\n" +
            "    " + regFunc + "(1, @" + hookDef + "(function(" + playerType + "@ " + playerParam + ") {\n" +
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
            "funcdef void " + hookDef + "(" + playerType + "@);\n" +
            "void " + regFunc + "(int hook, " + hookDef + "@ cb) {}\n" +
            "void " + mapActFunc + "()\n" +
            "{\n" +
            "    " + regFunc + "(1, @" + hookDef + "(function(" + playerType + "@ " + playerParam + ") {\n" +
            "        " + playerParam + "." + resetMethod + "(true, true);\n" +
            "    }));\n" +
            "}\n";

        const auto unguardedDiags = AnalyzeScript(unguardedCode);
        CHECK(HasNullDereferenceWarning(unguardedDiags, playerParam));
    }

    TEST_CASE("Local Handle Initialized to Null Emits Warning on Dereference")
    {
        const std::string typeName = GenerateRandomSymbolName("Entity");
        const std::string varName = GenerateRandomSymbolName("pEnt");
        const std::string methodName = GenerateRandomSymbolName("Spawn");
        const std::string funcName = GenerateRandomSymbolName("InitScene");

        const std::string unguardedCode =
            "class " + typeName + " { void " + methodName + "() {} }\n" +
            "void " + funcName + "()\n" +
            "{\n" +
            "    " + typeName + "@ " + varName + " = null;\n" +
            "    " + varName + "." + methodName + "();\n" +
            "}\n";

        const auto diags = AnalyzeScript(unguardedCode);
        CHECK(HasNullDereferenceWarning(diags, varName));

        const std::string guardedCode =
            "class " + typeName + " { void " + methodName + "() {} }\n" +
            "void " + funcName + "()\n" +
            "{\n" +
            "    " + typeName + "@ " + varName + " = null;\n" +
            "    if (" + varName + " !is null)\n" +
            "    {\n" +
            "        " + varName + "." + methodName + "();\n" +
            "    }\n" +
            "}\n";

        const auto guardedDiags = AnalyzeScript(guardedCode);
        CHECK_FALSE(HasNullDereferenceWarning(guardedDiags, varName));
    }

    TEST_CASE("Reverse Operand Null Checks with null !is handle and null == handle")
    {
        const std::string typeName = GenerateRandomSymbolName("Monster");
        const std::string paramName = GenerateRandomSymbolName("pMonster");
        const std::string methodName = GenerateRandomSymbolName("Killed");
        const std::string funcName = GenerateRandomSymbolName("OnTakeDamage");

        const std::string reverseIfNotCode =
            "class " + typeName + " { void " + methodName + "() {} }\n" +
            "void " + funcName + "(" + typeName + "@ " + paramName + ")\n" +
            "{\n" +
            "    if (null !is " + paramName + ")\n" +
            "    {\n" +
            "        " + paramName + "." + methodName + "();\n" +
            "    }\n" +
            "}\n";

        const auto reverseIfNotDiags = AnalyzeScript(reverseIfNotCode);
        CHECK_FALSE(HasNullDereferenceWarning(reverseIfNotDiags, paramName));

        const std::string reverseEarlyReturnCode =
            "class " + typeName + " { void " + methodName + "() {} }\n" +
            "void " + funcName + "(" + typeName + "@ " + paramName + ")\n" +
            "{\n" +
            "    if (null == " + paramName + ")\n" +
            "        return;\n" +
            "    " + paramName + "." + methodName + "();\n" +
            "}\n";

        const auto reverseEarlyDiags = AnalyzeScript(reverseEarlyReturnCode);
        CHECK_FALSE(HasNullDereferenceWarning(reverseEarlyDiags, paramName));
    }

    TEST_CASE("Handle Reassigned to Null Emits Warning on Subsequent Dereference")
    {
        const std::string typeName = GenerateRandomSymbolName("Target");
        const std::string paramName = GenerateRandomSymbolName("pTarget");
        const std::string methodName = GenerateRandomSymbolName("Execute");
        const std::string funcName = GenerateRandomSymbolName("RunPass");

        const std::string code =
            "class " + typeName + " { void " + methodName + "() {} }\n" +
            "void " + funcName + "(" + typeName + "@ " + paramName + ")\n" +
            "{\n" +
            "    if (" + paramName + " !is null)\n" +
            "    {\n" +
            "        " + paramName + "." + methodName + "();\n" +
            "        @" + paramName + " = null;\n" +
            "        " + paramName + "." + methodName + "();\n" +
            "    }\n" +
            "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK(HasNullDereferenceWarning(diags, paramName));
    }

    TEST_CASE("Constructed Instance Initialization Is Known NonNull")
    {
        const std::string typeName = GenerateRandomSymbolName("Buffer");
        const std::string varName = GenerateRandomSymbolName("pBuf");
        const std::string methodName = GenerateRandomSymbolName("Clear");
        const std::string funcName = GenerateRandomSymbolName("Allocate");

        const std::string code =
            "class " + typeName + " { void " + methodName + "() {} }\n" +
            "void " + funcName + "()\n" +
            "{\n" +
            "    " + typeName + "@ " + varName + " = " + typeName + "();\n" +
            "    " + varName + "." + methodName + "();\n" +
            "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags, varName));
    }

    TEST_CASE("Sven Co-op Style Inequality Operator != null Guards Dereference")
    {
        const std::string typeName = GenerateRandomSymbolName("CBaseEntity");
        const std::string paramName = GenerateRandomSymbolName("pActivator");
        const std::string methodName = GenerateRandomSymbolName("Use");
        const std::string funcName = GenerateRandomSymbolName("Touch");

        const std::string code =
            "class " + typeName + " { void " + methodName + "() {} }\n" +
            "void " + funcName + "(" + typeName + "@ " + paramName + ")\n" +
            "{\n" +
            "    if (" + paramName + " != null)\n" +
            "    {\n" +
            "        " + paramName + "." + methodName + "();\n" +
            "    }\n" +
            "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags, paramName));
    }

    TEST_CASE("Call Argument Expression Dereference Warning")
    {
        const std::string hostType = GenerateRandomSymbolName("Host");
        const std::string argType = GenerateRandomSymbolName("Item");
        const std::string hostVar = GenerateRandomSymbolName("pHost");
        const std::string argVar = GenerateRandomSymbolName("pItem");
        const std::string getItemMethod = GenerateRandomSymbolName("GetSubItem");
        const std::string acceptMethod = GenerateRandomSymbolName("AcceptItem");
        const std::string funcName = GenerateRandomSymbolName("ProcessItem");

        const std::string code =
            "class " + argType + " { " + argType + "@ " + getItemMethod + "() { return null; } }\n" +
            "class " + hostType + " { void " + acceptMethod + "(" + argType + "@ item) {} }\n" +
            "void " + funcName + "(" + hostType + "@ " + hostVar + ", " + argType + "@ " + argVar + ")\n" +
            "{\n" +
            "    if (" + hostVar + " !is null)\n" +
            "    {\n" +
            "        " + hostVar + "." + acceptMethod + "(" + argVar + "." + getItemMethod + "());\n" +
            "    }\n" +
            "}\n";

        const auto diags = AnalyzeScript(code);
        CHECK_FALSE(HasNullDereferenceWarning(diags, hostVar));
        CHECK(HasNullDereferenceWarning(diags, argVar));
    }

    TEST_CASE("Oracle Parity - Handle Comparison Equality Warning vs Identity Clean")
    {
        // Measured against angelscript_oracle:
        // 1. `handle is null` / `handle !is null` compiles with 0 warnings.
        // 2. `null == handle` / `handle == null` compiles with WARNING:
        //    "The operand is implicitly converted to handle in order to compare them"
        // Both forms act as valid flow-sensitive guards against null dereferences in AngelLSP.
        const std::string typeName = GenerateRandomSymbolName("Entity");
        const std::string handleVar = GenerateRandomSymbolName("pEnt");
        const std::string method = GenerateRandomSymbolName("Init");
        const std::string fn = GenerateRandomSymbolName("Check");

        const std::string isNullScript =
            "class " + typeName + " { void " + method + "() {} }\n" +
            "void " + fn + "(" + typeName + "@ " + handleVar + ")\n" +
            "{\n" +
            "    if (" + handleVar + " !is null)\n" +
            "        " + handleVar + "." + method + "();\n" +
            "}\n";
        CHECK_FALSE(HasNullDereferenceWarning(AnalyzeScript(isNullScript), handleVar));

        const std::string eqNullScript =
            "class " + typeName + " { void " + method + "() {} }\n" +
            "void " + fn + "(" + typeName + "@ " + handleVar + ")\n" +
            "{\n" +
            "    if (null == " + handleVar + ")\n" +
            "        return;\n" +
            "    " + handleVar + "." + method + "();\n" +
            "}\n";
        CHECK_FALSE(HasNullDereferenceWarning(AnalyzeScript(eqNullScript), handleVar));
    }
TEST_SUITE_END();
} // namespace angel_lsp::test
