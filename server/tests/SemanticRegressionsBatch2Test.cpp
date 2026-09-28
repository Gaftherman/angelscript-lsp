/**
 * @file SemanticRegressionsBatch2Test.cpp
 * @brief Empirical reproduction and forensic verification suite for the 11 semantic issues
 *        reported in Sven Co-op scripts and meta_api::json.
 */

#include <doctest/doctest.h>

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

namespace angel_lsp::test
{
namespace
{
/**
 * @brief Analyzes a script snippet, optionally with predefined stubs, and returns all diagnostics.
 * @param[in] code AngelScript source text.
 * @param[in] predefinedCode Optional predefined stub source text.
 * @return Vector of emitted diagnostics.
 */
std::vector<analysis::Diagnostic> AnalyzeSnippetWithPredefined(const std::string& code,
                                                             const std::string& predefinedCode = "")
{
    parser::AngelScriptParser parser;
    analysis::SymbolTable table;
    analysis::SymbolCollector collector(nullptr);

    if (!predefinedCode.empty())
    {
        collector.CollectSymbols("file:///engine.as.predefined", predefinedCode, parser, table);
    }

    auto diags = collector.CollectSymbols("file:///test.as", code, parser, table);

    analysis::LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(code, parser);

    TSTree* tree = parser.Parse(code);

    analysis::SemanticAnalyzer analyzer(nullptr);
    analysis::SemanticAnalysisRequest request{table, "file:///test.as", ".as.predefined", nullptr};
    request.sourceCode = code;
    request.tree = tree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());
    if (tree)
    {
        ts_tree_delete(tree);
    }
    return diags;
}

/**
 * @brief Checks if any diagnostic in the list has the given diagnostic code.
 * @param[in] diags Vector of diagnostics.
 * @param[in] code Diagnostic code string.
 * @return True if code is found.
 */
bool HasDiagCode(const std::vector<analysis::Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(), [code](const analysis::Diagnostic& d) { return d.code == code; });
}

void DumpDiags(std::string_view label, const std::vector<analysis::Diagnostic>& diags)
{
    for (const auto& d : diags)
    {
        MESSAGE("[" << label << "] emitted code: " << d.code << " -> " << d.message);
    }
}
} // namespace

TEST_SUITE_BEGIN("SemanticRegressionBatch2");

TEST_CASE("Vector 1 - Conditional assignment null flow inside conjunction recognizes non-null")
{
    const std::string baseName = GenerateRandomSymbolName("CBaseEntity");
    const std::string derivedName = GenerateRandomSymbolName("CBaseMonster");
    const std::string varAiment = GenerateRandomSymbolName("aiment");
    const std::string varMonster = GenerateRandomSymbolName("monster");

    const std::string code =
        "class " + baseName + " { bool IsMonster() { return true; } }\n" +
        "class " + derivedName + " : " + baseName + " { bool IsPlayer() { return false; } }\n" +
        "void Test(" + baseName + "@ " + varAiment + ") {\n" +
        "    " + derivedName + "@ " + varMonster + " = null;\n" +
        "    if (" + varAiment + " !is null && " + varAiment + ".IsMonster() && (@" + varMonster + " = cast<" + derivedName + "@>(" +
        varAiment + ")) !is null) {\n" +
        "        " + varMonster + ".IsPlayer();\n" +
        "    }\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 1", diags);
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::PossibleNullDereference));
}

TEST_CASE("Vector 2 - Disjunctive short-circuit null flow recognizes non-null on both branches")
{
    const std::string entityName = GenerateRandomSymbolName("CBaseEntity");
    const std::string traceName = GenerateRandomSymbolName("TraceResult");
    const std::string funcsName = GenerateRandomSymbolName("EntityFuncs");
    const std::string varTr = GenerateRandomSymbolName("tr");
    const std::string varHit = GenerateRandomSymbolName("hit");
    const std::string globalFuncs = GenerateRandomSymbolName("g_EntityFuncs");

    const std::string code =
        "class " + entityName + " { bool IsPlayer() { return false; } }\n" +
        "class " + traceName + " { " + entityName + "@ pHit; }\n" +
        "class " + funcsName + " { " + entityName + "@ Instance(" + entityName + "@ e) { return e; } }\n" +
        funcsName + " " + globalFuncs + ";\n" +
        "void Test(" + traceName + " " + varTr + ") {\n" +
        "    " + entityName + "@ " + varHit + " = null;\n" +
        "    if (" + varHit + " !is null || (" + varTr + ".pHit !is null && (@" + varHit + " = " +
        globalFuncs + ".Instance(" + varTr + ".pHit)) !is null)) {\n" +
        "        " + varHit + ".IsPlayer();\n" +
        "    }\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 2", diags);
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::PossibleNullDereference));
}

TEST_CASE("Vector 3 - Direct-init constructor with const string&in parameter resolves cleanly")
{
    const std::string loggerClass = GenerateRandomSymbolName("Logger");
    const std::string varLogger = GenerateRandomSymbolName("g_Logger");

    const std::string code =
        "class " + loggerClass + " {\n" +
        "    " + loggerClass + "(const string&in Name) {}\n" +
        "}\n" +
        "void Test() {\n" +
        "    " + loggerClass + " " + varLogger + "(\"JSON\");\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 3", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-matching-constructor"));
}

TEST_CASE("Vector 4 - Direct-init constructor with default argument resolves when argument provided")
{
    const std::string validatorClass = GenerateRandomSymbolName("Validator");
    const std::string varVal = GenerateRandomSymbolName("validator");
    const std::string varStrict = GenerateRandomSymbolName("strict");

    const std::string code =
        "class " + validatorClass + " {\n" +
        "    " + validatorClass + "(bool strict = false) {}\n" +
        "}\n" +
        "void Test() {\n" +
        "    bool " + varStrict + " = true;\n" +
        "    " + validatorClass + " " + varVal + "(" + varStrict + ");\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 4", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-matching-constructor"));
}

TEST_CASE("Vector 5 - Overload ranking chooses float over int when adding float and numeric literal")
{
    const std::string customClass = GenerateRandomSymbolName("CustomEntity");
    const std::string engineClass = GenerateRandomSymbolName("Engine");
    const std::string varCustom = GenerateRandomSymbolName("pCustom");
    const std::string globalEngine = GenerateRandomSymbolName("g_Engine");

    const std::string code =
        "class " + customClass + " {\n" +
        "    void SetKeyvalue(const string&in key, float value) {}\n" +
        "    void SetKeyvalue(const string&in key, int value) {}\n" +
        "}\n" +
        "class " + engineClass + " {\n" +
        "    float time;\n" +
        "}\n" +
        engineClass + " " + globalEngine + ";\n" +
        "void Test(" + customClass + "@ " + varCustom + ") {\n" +
        "    " + varCustom + ".SetKeyvalue(\"shield\", " + globalEngine + ".time + 0.1);\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 5", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
}

TEST_CASE("Vector 6 - Math.max numeric overload resolves cleanly without spurious ambiguity")
{
    const std::string mathNs = GenerateRandomSymbolName("Math");
    const std::string engineClass = GenerateRandomSymbolName("Engine");
    const std::string globalEngine = GenerateRandomSymbolName("g_Engine");
    const std::string varSubsequent = GenerateRandomSymbolName("subsequent");

    const std::string code =
        "namespace " + mathNs + " {\n" +
        "    float max(float a, float b) { return a; }\n" +
        "    int64 max(int64 a, int64 b) { return a; }\n" +
        "    uint64 max(uint64 a, uint64 b) { return a; }\n" +
        "}\n" +
        "class " + engineClass + " { float time; }\n" +
        engineClass + " " + globalEngine + ";\n" +
        "float Test() {\n" +
        "    float " + varSubsequent + " = 0.5f;\n" +
        "    float val = " + mathNs + ".max(0.1, (" + globalEngine + ".time > 0.0f ? 1.0 : " + varSubsequent + "));\n" +
        "    return val;\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 6", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
    CHECK(diags.empty());
}

TEST_CASE("Vector 7 - Enum values implicitly promote to float in call arguments")
{
    const std::string enumName = GenerateRandomSymbolName("SOUND_CHANNEL");
    const std::string fnName = GenerateRandomSymbolName("PlaySound");

    const std::string code =
        "enum " + enumName + " {\n" +
        "    CHAN_AUTO = 0,\n" +
        "    CHAN_WEAPON = 1\n" +
        "}\n" +
        "void " + fnName + "(float channel) {}\n" +
        "void Test() {\n" +
        "    " + fnName + "(CHAN_WEAPON);\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 7", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-explicit-conversion"));
    CHECK(diags.empty());
}

TEST_CASE("Vector 8 - Ternary expression retains array bracket type")
{
    const std::string attackEnum = GenerateRandomSymbolName("AttackType");
    const std::string gunClass = GenerateRandomSymbolName("GunParams");
    const std::string playerClass = GenerateRandomSymbolName("Player");
    const std::string weaponsNs = GenerateRandomSymbolName("weapons");

    const std::string code =
        "enum " + attackEnum + " { Primary, Secondary }\n" +
        "class " + gunClass + " {\n" +
        "    float[] primary_accuracy;\n" +
        "    float[] secondary_accuracy;\n" +
        "}\n" +
        "class " + playerClass + " {}\n" +
        "namespace " + weaponsNs + " {\n" +
        "    float Accuracy(" + playerClass + "@ p, float[] acc) { return 0.0f; }\n" +
        "}\n" +
        "float Test(" + playerClass + "@ player, " + gunClass + " gp, " + attackEnum + " type) {\n" +
        "    float cone = " + weaponsNs + "::Accuracy(player, (type == " + attackEnum + "::Primary) ? gp.primary_accuracy : gp.secondary_accuracy);\n" +
        "    return cone;\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 8", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-explicit-conversion"));
    CHECK(diags.empty());
}

TEST_CASE("Vector 9 - Multi-hop base class resolution across predefined stubs")
{
    const std::string stubMonster = GenerateRandomSymbolName("ScriptBaseMonsterEntity");
    const std::string taskClass = GenerateRandomSymbolName("Task");
    const std::string schedClass = GenerateRandomSymbolName("Schedule");
    const std::string scriptBase = GenerateRandomSymbolName("bts_rc_base_monster");
    const std::string parasiteClass = GenerateRandomSymbolName("monster_parasite");

    const std::string predefinedCode =
        "class " + taskClass + " {}\n" +
        "class " + schedClass + " {}\n" +
        "class " + stubMonster + " {\n" +
        "    void RunTask(" + taskClass + "@ pTask) {}\n" +
        "    " + schedClass + "@ m_Schedules;\n" +
        "    " + stubMonster + "@ BaseClass;\n" +
        "}\n";

    const std::string scriptCode =
        "class " + scriptBase + " : " + stubMonster + " {}\n" +
        "class " + parasiteClass + " : " + scriptBase + " {\n" +
        "    void CustomMethod(" + taskClass + "@ pTask) {\n" +
        "        @this.m_Schedules = null;\n" +
        "        BaseClass.RunTask(pTask);\n" +
        "    }\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(scriptCode, predefinedCode);
    DumpDiags("Vector 9", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-warn-undeclared-identifier"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));

    // Invariant: BaseClass is not a language keyword; without an explicit declaration it must be flagged
    const std::string cleanBase = GenerateRandomSymbolName("PureBase");
    const std::string cleanDerived = GenerateRandomSymbolName("PureDerived");
    const std::string cleanCode =
        "class " + cleanBase + " { void RunTask() {} }\n" +
        "class " + cleanDerived + " : " + cleanBase + " {\n" +
        "    void Test() {\n" +
        "        BaseClass.RunTask();\n" +
        "    }\n" +
        "}\n";
    const auto cleanDiags = AnalyzeSnippetWithPredefined(cleanCode);
    CHECK(HasDiagCode(cleanDiags, "as-warn-undeclared-identifier"));
}

TEST_CASE("Vector 10 - L-value output parameter resolves on private class member")
{
    const std::string jsonClass = GenerateRandomSymbolName("json");
    const std::string fnDeserialize = GenerateRandomSymbolName("Deserialize");
    const std::string mgrClass = GenerateRandomSymbolName("ConfigManager");

    const std::string code =
        "class " + jsonClass + " {}\n" +
        "void " + fnDeserialize + "(string config, " + jsonClass + "@ &out target) {}\n" +
        "class " + mgrClass + " {\n" +
        "    private " + jsonClass + "@ m_defaults;\n" +
        "    string __GetDefaultConfig__() { return \"\"; }\n" +
        "    void Init() {\n" +
        "        " + fnDeserialize + "(this.__GetDefaultConfig__(), m_defaults);\n" +
        "    }\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 10", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-err-lvalue-required-for-out-param"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("Vector 11 - Multi-segment namespace type and switch CFG exhaustiveness")
{
    const std::string nsRoot = GenerateRandomSymbolName("meta_api");
    const std::string nsSub = GenerateRandomSymbolName("json");
    const std::string nullClass = GenerateRandomSymbolName("Null");
    const std::string enumType = GenerateRandomSymbolName("JsonType");
    const std::string valueClass = GenerateRandomSymbolName("JsonValue");

    const std::string code =
        "namespace " + nsRoot + " {\n" +
        "    namespace " + nsSub + " {\n" +
        "        namespace v2 {\n" +
        "            class " + nullClass + " {}\n" +
        "        }\n" +
        "    }\n" +
        "}\n" +
        "enum " + enumType + " { JT_Null, JT_Bool }\n" +
        "class " + valueClass + " {\n" +
        "    " + enumType + " Type;\n" +
        "    bool Check(const " + nsRoot + "::" + nsSub + "::v2::" + nullClass + "&in value) {\n" +
        "        switch(this.Type) {\n" +
        "            case JT_Null: return true;\n" +
        "            default: return false;\n" +
        "        }\n" +
        "    }\n" +
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(code);
    DumpDiags("Vector 11", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-err-unresolved-type"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-not-all-paths-return"));
}

TEST_CASE("Named arguments - Optional parameter skipped and named parameter provided")
{
    const std::string predefined =
        "class string {};\n"
        "class CBaseEntity {};\n"
        "class dictionary {};\n"
        "class CEntityFuncs {\n"
        "    CBaseEntity@ CreateEntity(const string& in szClassName, dictionary@ pDictionary = null, bool fSpawn = true);\n"
        "};\n"
        "CEntityFuncs g_EntityFuncs;\n";

    const std::string script =
        "void Test() {\n"
        "    string szAmmoName = \"weapon_9mmclip\";\n"
        "    CBaseEntity@ pClip = g_EntityFuncs.CreateEntity(szAmmoName, fSpawn: false);\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(script, predefined);
    DumpDiags("Named Args Test", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-warn-undeclared-identifier"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("Named arguments - Parameter label does not mark local variable as used")
{
    const std::string fSpawnVar = GenerateRandomSymbolName("fSpawn");
    const std::string fnName = GenerateRandomSymbolName("MakeItem");

    const std::string predefined =
        "class string {};\n"
        "void " + fnName + "(string name, bool " + fSpawnVar + " = true) {}\n";

    const std::string script =
        "void Test() {\n"
        "    bool " + fSpawnVar + " = false;\n" // Local variable with same name as parameter
        "    " + fnName + "(\"test\", " + fSpawnVar + ": true);\n" // Argument label should NOT count as using the local variable
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(script, predefined);
    DumpDiags("Named Arg Local Var Shadow", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-warn-undeclared-identifier"));
    CHECK(HasDiagCode(diags, "as-warn-unused-variable"));
}

TEST_CASE("Named arguments - Multi-parameter reordered invocation with container initializer list")
{
    const std::string fnName = GenerateRandomSymbolName("CustomFunct");
    const std::string idParam = GenerateRandomSymbolName("id");
    const std::string fParam = GenerateRandomSymbolName("f");
    const std::string argSParam = GenerateRandomSymbolName("argS");

    const std::string predefined =
        "class string {};\n"
        "template <typename T> class array {};\n"
        "void " + fnName + "(int " + idParam + " = 0, bool " + fParam + " = true, array<string> " + argSParam + " = array<string>()) {}\n";

    const std::string script =
        "void Test() {\n"
        "    " + fnName + "(" + argSParam + ": {\"hi\", \"hellol\"}, " + idParam + ": 1, " + fParam + ": false);\n"
        "    " + fnName + "(" + argSParam + ": {\"only\"});\n"
        "    " + fnName + "(10, " + argSParam + ": {\"mixed\"});\n"
        "    " + fnName + "(" + fParam + ": false, " + idParam + ": 42);\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(script, predefined);
    DumpDiags("Multi-Param Reordered Named Args", diags);
    CHECK_FALSE(HasDiagCode(diags, "as-warn-undeclared-identifier"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-argument-count"));
}

TEST_CASE("Named arguments - Rejections for duplicate, conflict, and unknown names")
{
    const std::string fnName = GenerateRandomSymbolName("RejectionTestFn");
    const std::string idParam = GenerateRandomSymbolName("id");
    const std::string fParam = GenerateRandomSymbolName("f");

    const std::string predefined =
        "void " + fnName + "(int " + idParam + ", bool " + fParam + ") {}\n";

    // Duplicate named argument
    const std::string scriptDupe =
        "void TestDupe() {\n"
        "    " + fnName + "(" + idParam + ": 1, " + idParam + ": 2);\n"
        "}\n";
    const auto diagsDupe = AnalyzeSnippetWithPredefined(scriptDupe, predefined);
    CHECK(HasDiagCode(diagsDupe, "as-err-call-no-matching-signature"));

    // Positional argument conflicting with named argument
    const std::string scriptConflict =
        "void TestConflict() {\n"
        "    " + fnName + "(1, " + idParam + ": 2);\n"
        "}\n";
    const auto diagsConflict = AnalyzeSnippetWithPredefined(scriptConflict, predefined);
    CHECK(HasDiagCode(diagsConflict, "as-err-call-no-matching-signature"));

    // Positional argument after named argument
    const std::string scriptPosAfterNamed =
        "void TestPosAfterNamed() {\n"
        "    " + fnName + "(" + idParam + ": 1, true);\n"
        "}\n";
    const auto diagsPosAfterNamed = AnalyzeSnippetWithPredefined(scriptPosAfterNamed, predefined);
    CHECK(HasDiagCode(diagsPosAfterNamed, "as-err-positional-after-named-arg"));

    // Unknown named argument
    const std::string scriptUnknown =
        "void TestUnknown() {\n"
        "    " + fnName + "(unknownParam: 1, " + fParam + ": true);\n"
        "}\n";
    const auto diagsUnknown = AnalyzeSnippetWithPredefined(scriptUnknown, predefined);
    CHECK(HasDiagCode(diagsUnknown, "as-err-call-no-matching-signature"));
}

TEST_CASE("Named arguments - Incompatible element in named initializer list emits diagnostic")
{
    const std::string fnName = GenerateRandomSymbolName("InitListMismatchFn");
    const std::string argSParam = GenerateRandomSymbolName("argS");
    const std::string customClass = GenerateRandomSymbolName("MyCustomClass");

    const std::string predefined =
        "class string {};\n"
        "template <typename T> class array {};\n"
        "class " + customClass + " {};\n"
        "void " + fnName + "(int id = 0, array<string> " + argSParam + " = array<string>()) {}\n";

    const std::string script =
        "void Test() {\n"
        "    " + customClass + " customObj;\n"
        "    " + fnName + "(" + argSParam + ": {customObj});\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithPredefined(script, predefined);
    DumpDiags("Named Init List Element Mismatch", diags);
    CHECK(HasDiagCode(diags, "as-err-no-implicit-conversion"));
}

TEST_CASE("SvenCoop v2.as - this.Get overload resolution with mutable ref")
{
    const std::string v2Path = "E:/Github/src/bts_rc/scripts/mikk155/meta_api/json/v2.as";
    std::ifstream f(v2Path);
    if (!f.is_open())
    {
        MESSAGE("v2.as not found, skipping direct file analysis");
        return;
    }
    std::string script((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    std::string predefined;
    std::ifstream pf("E:/Github/src/bts_rc/as.predefined");
    if (pf.is_open())
    {
        predefined.assign((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
    }

    parser::AngelScriptParser parser;
    analysis::SymbolTable table;
    analysis::SymbolCollector collector(nullptr);

    if (!predefined.empty())
    {
        collector.CollectSymbols("file:///E:/Github/src/bts_rc/as.predefined", predefined, parser, table);
    }

    std::ifstream jf("E:/Github/src/bts_rc/scripts/mikk155/meta_api/json.as");
    if (jf.is_open())
    {
        std::string jsonCode((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
        collector.CollectSymbols("file:///E:/Github/src/bts_rc/scripts/mikk155/meta_api/json.as", jsonCode, parser, table);
    }

    auto diags = collector.CollectSymbols("file:///E:/Github/src/bts_rc/scripts/mikk155/meta_api/json/v2.as", script, parser, table);

    analysis::LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(script, parser);

    TSTree* tree = parser.Parse(script);

    analysis::SemanticAnalyzer analyzer(nullptr);
    analysis::SemanticAnalysisRequest request{table, "file:///E:/Github/src/bts_rc/scripts/mikk155/meta_api/json/v2.as", ".as.predefined", nullptr};
    request.sourceCode = script;
    request.tree = tree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());
    if (tree)
    {
        ts_tree_delete(tree);
    }
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
}

TEST_CASE("Overload resolution - Mutable reference with default bool does not report ambiguity")
{
    const std::string cls = test::GenerateRandomSymbolName("CVal");
    const std::string getFn = test::GenerateRandomSymbolName("Get");

    std::string code =
        "class " + cls + " {\n"
        "    bool " + getFn + "(const string& in k, int& out val, bool strict = false) const { return false; }\n"
        "    bool " + getFn + "(const string& in k, float& out val, bool strict = false) const { return false; }\n"
        "    bool " + getFn + "(const string& in k, bool& out val, bool strict = false) const { return false; }\n"
        "    bool " + getFn + "(const string& in k, string& out val, bool strict = false) const { return false; }\n"
        "    void Test() {\n"
        "        bool temp = false;\n"
        "        bool strict = true;\n"
        "        this." + getFn + "(\"key\", temp, strict);\n"
        "    }\n"
        "};\n";

    parser::AngelScriptParser parser;
    analysis::SymbolTable table;
    analysis::SymbolCollector collector(nullptr);
    auto diags = collector.CollectSymbols("file:///test.as", code, parser, table);

    analysis::LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(code, parser);
    TSTree* tree = parser.Parse(code);

    analysis::SemanticAnalyzer analyzer(nullptr);
    analysis::SemanticAnalysisRequest request{table, "file:///test.as", ".as.predefined", nullptr};
    request.sourceCode = code;
    request.tree = tree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());
    if (tree)
    {
        ts_tree_delete(tree);
    }
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
}

TEST_CASE("Named arguments - Variable name matches parameter name")
{
    const std::string fnName = test::GenerateRandomSymbolName("func");
    const std::string p0 = test::GenerateRandomSymbolName("id");
    const std::string p1 = test::GenerateRandomSymbolName("flag");
    const std::string p2 = test::GenerateRandomSymbolName("argList");

    std::string code =
        "int " + p0 + " = 1;\n"
        "bool " + p1 + " = true;\n"
        "array<string> " + p2 + " = {};\n"
        "void " + fnName + "(int " + p0 + " = 0, bool " + p1 + " = true, array<string> " + p2 + " = array<string>()) {}\n"
        "void main() {\n"
        "    " + fnName + "(" + p2 + ": " + p2 + ", " + p0 + ": " + p0 + ", " + p1 + ": " + p1 + ");\n"
        "}\n";

    parser::AngelScriptParser parser;
    analysis::SymbolTable table;
    analysis::SymbolCollector collector(nullptr);
    auto diags = collector.CollectSymbols("file:///test.as", code, parser, table);

    analysis::LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(code, parser);
    TSTree* tree = parser.Parse(code);

    analysis::SemanticAnalyzer analyzer(nullptr);
    analysis::SemanticAnalysisRequest request{table, "file:///test.as", ".as.predefined", nullptr};
    request.sourceCode = code;
    request.tree = tree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());
    if (tree)
    {
        ts_tree_delete(tree);
    }
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
}

TEST_CASE("Named arguments - Non-existent parameter name emits no matching signature error")
{
    const std::string fnName = test::GenerateRandomSymbolName("func");
    const std::string p0 = test::GenerateRandomSymbolName("realParam");
    const std::string fakeParam = test::GenerateRandomSymbolName("fakeParam");

    std::string code =
        "void " + fnName + "(int " + p0 + " = 0) {}\n"
        "void main() {\n"
        "    " + fnName + "(" + fakeParam + ": 123);\n"
        "}\n";

    parser::AngelScriptParser parser;
    analysis::SymbolTable table;
    analysis::SymbolCollector collector(nullptr);
    auto diags = collector.CollectSymbols("file:///test.as", code, parser, table);

    analysis::LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(code, parser);
    TSTree* tree = parser.Parse(code);

    analysis::SemanticAnalyzer analyzer(nullptr);
    analysis::SemanticAnalysisRequest request{table, "file:///test.as", ".as.predefined", nullptr};
    request.sourceCode = code;
    request.tree = tree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());
    if (tree)
    {
        ts_tree_delete(tree);
    }
    CHECK(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_SUITE_END();

} // namespace angel_lsp::test
