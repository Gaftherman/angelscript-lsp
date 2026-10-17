#include <doctest/doctest.h>

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "config/EngineRuleConfig.h"
#include "config/ServerConfig.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;
namespace codes = angel_lsp::diagnostics::codes;

namespace
{

bool HasDiag(const std::vector<Diagnostic>& diags, std::string_view code, const std::string& name)
{
    return std::any_of(diags.begin(), diags.end(), [&](const Diagnostic& d)
                       { return d.code == code && (name.empty() || d.message.find(name) != std::string::npos); });
}

std::vector<Diagnostic> AnalyzeSnippet(const std::string& code,
                                       const angel_lsp::config::EngineRuleConfig* engineRules = nullptr,
                                       const std::string& fileUri = "file:///test.as",
                                       const angel_lsp::config::DiagnosticsConfig* diagConfig = nullptr)
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    auto diags = collector.CollectSymbols({fileUri, code, &i18n}, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    request.tree = parser.Parse(code);
    request.engineRules = engineRules;
    request.diagnostics = diagConfig;

    SemanticAnalyzer analyzer(nullptr);
    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());

    if (request.tree)
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    return diags;
}

std::vector<Diagnostic> AnalyzeSnippetWithPredefined(const std::string& code,
                                                     const angel_lsp::config::EngineRuleConfig* engineRules,
                                                     const std::string& predefinedCode)
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    if (!predefinedCode.empty())
    {
        collector.CollectSymbols({"file:///engine.as.predefined", predefinedCode, &i18n}, parser, table);
    }

    const std::string fileUri = "file:///test.as";
    auto diags = collector.CollectSymbols({fileUri, code, &i18n}, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    request.tree = parser.Parse(code);
    request.engineRules = engineRules;

    SemanticAnalyzer analyzer(nullptr);
    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());

    if (request.tree)
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    return diags;
}

} // namespace

TEST_CASE("UnusedSymbolChecker - local variables in function and mixin method")
{
    const std::string unusedLocal = angel_lsp::test::GenerateRandomSymbolName("locVar");
    const std::string usedLocal = angel_lsp::test::GenerateRandomSymbolName("usedLoc");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("Mix");
    const std::string unusedMixinLocal = angel_lsp::test::GenerateRandomSymbolName("mixLoc");

    const std::string code = "void TestFunc() {\n"
                             "    int " +
                             unusedLocal +
                             " = 1;\n"
                             "    int " +
                             usedLocal +
                             " = 2;\n"
                             "    " +
                             usedLocal + " = " + usedLocal +
                             " + 1;\n"
                             "}\n"
                             "mixin class " +
                             mixinName +
                             " {\n"
                             "    void MixinMethod() {\n"
                             "        int " +
                             unusedMixinLocal +
                             " = 10;\n"
                             "    }\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code);
    CHECK(HasDiag(diags, codes::UnusedVariable, unusedLocal));
    CHECK(HasDiag(diags, codes::UnusedVariable, unusedMixinLocal));
    CHECK_FALSE(HasDiag(diags, codes::UnusedVariable, usedLocal));
}

TEST_CASE("UnusedSymbolChecker - member fields in class and mixin")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;

    const std::string className = angel_lsp::test::GenerateRandomSymbolName("MyClass");
    const std::string unusedField = angel_lsp::test::GenerateRandomSymbolName("m_unused");
    const std::string usedField = angel_lsp::test::GenerateRandomSymbolName("m_used");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("MyMixin");
    const std::string unusedMixinField = angel_lsp::test::GenerateRandomSymbolName("m_mixUnused");

    const std::string code = "class " + className +
                             " {\n"
                             "    int " +
                             unusedField +
                             ";\n"
                             "    int " +
                             usedField +
                             ";\n"
                             "    void DoWork() { " +
                             usedField +
                             " = 5; }\n"
                             "}\n"
                             "mixin class " +
                             mixinName +
                             " {\n"
                             "    int " +
                             unusedMixinField +
                             ";\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK(HasDiag(diags, codes::UnusedField, unusedField));
    CHECK(HasDiag(diags, codes::UnusedField, unusedMixinField));
    CHECK_FALSE(HasDiag(diags, codes::UnusedField, usedField));
}

TEST_CASE("UnusedSymbolChecker - global variables")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;

    const std::string unusedGlobal = angel_lsp::test::GenerateRandomSymbolName("g_unused");
    const std::string usedGlobal = angel_lsp::test::GenerateRandomSymbolName("g_used");

    const std::string code = "int " + unusedGlobal +
                             " = 42;\n"
                             "int " +
                             usedGlobal +
                             " = 100;\n"
                             "void Worker() {\n"
                             "    " +
                             usedGlobal +
                             "++;\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK(HasDiag(diags, codes::UnusedGlobalVariable, unusedGlobal));
    CHECK_FALSE(HasDiag(diags, codes::UnusedGlobalVariable, usedGlobal));
}

TEST_CASE("UnusedSymbolChecker - functions and methods")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;

    const std::string unusedFunc = angel_lsp::test::GenerateRandomSymbolName("UnusedFn");
    const std::string usedFunc = angel_lsp::test::GenerateRandomSymbolName("UsedFn");
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("WorkerCls");
    const std::string unusedPrivateMethod = angel_lsp::test::GenerateRandomSymbolName("PrivateHelper");
    const std::string unusedPublicMethod = angel_lsp::test::GenerateRandomSymbolName("DeadPublicApi");
    const std::string publicMethod = angel_lsp::test::GenerateRandomSymbolName("PublicApi");

    const std::string code = "void " + unusedFunc +
                             "() {}\n"
                             "void " +
                             usedFunc +
                             "() {}\n"
                             "class " +
                             className +
                             " {\n"
                             "    private void " +
                             unusedPrivateMethod +
                             "() {}\n"
                             "    public void " +
                             unusedPublicMethod +
                             "() {}\n"
                             "    public void " +
                             publicMethod +
                             "() {}\n"
                             "}\n"
                             "void MainEntry() {\n"
                             "    " +
                             usedFunc +
                             "();\n"
                             "    " +
                             className +
                             " inst;\n"
                             "    inst." +
                             publicMethod +
                             "();\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK(HasDiag(diags, codes::UnusedFunction, unusedFunc));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, usedFunc));
    CHECK(HasDiag(diags, codes::UnusedMethod, unusedPrivateMethod));
    CHECK(HasDiag(diags, codes::UnusedMethod, unusedPublicMethod));
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, publicMethod));
}

TEST_CASE("UnusedSymbolChecker - classes usage and inheritance")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;

    const std::string unusedClass = angel_lsp::test::GenerateRandomSymbolName("DeadClass");
    const std::string baseClass = angel_lsp::test::GenerateRandomSymbolName("BaseCls");
    const std::string derivedClass = angel_lsp::test::GenerateRandomSymbolName("DerivedCls");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("ComponentMixin");
    const std::string hostClass = angel_lsp::test::GenerateRandomSymbolName("HostCls");

    const std::string code = "class " + unusedClass +
                             " {}\n"
                             "class " +
                             baseClass +
                             " {}\n"
                             "class " +
                             derivedClass + " : " + baseClass +
                             " {}\n"
                             "mixin class " +
                             mixinName +
                             " {}\n"
                             "class " +
                             hostClass + " : " + mixinName +
                             " {}\n"
                             "void Execute() {\n"
                             "    " +
                             derivedClass +
                             " d;\n"
                             "    " +
                             hostClass +
                             " h;\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK(HasDiag(diags, codes::UnusedClass, unusedClass));
    CHECK_FALSE(HasDiag(diags, codes::UnusedClass, baseClass));
    CHECK_FALSE(HasDiag(diags, codes::UnusedClass, derivedClass));
    CHECK_FALSE(HasDiag(diags, codes::UnusedClass, mixinName));
    CHECK_FALSE(HasDiag(diags, codes::UnusedClass, hostClass));
}

TEST_CASE("UnusedSymbolChecker - engine entity and lifecycle methods exemption")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.ignoredBaseClasses = {"ScriptBasePlayerAmmoEntity", "ScriptBaseEntity"};
    rules.unusedRules.lifecycleMethods = {"Spawn", "Precache", "KeyValue"};
    rules.unusedRules.ignoredGlobalFunctions = {"MapInit", "PluginInit"};

    const std::string entityClass = angel_lsp::test::GenerateRandomSymbolName("AmmoEntity");

    const std::string code = "class " + entityClass +
                             " : ScriptBasePlayerAmmoEntity {\n"
                             "    void Spawn() {}\n"
                             "    void Precache() {}\n"
                             "    void KeyValue() {}\n"
                             "}\n"
                             "void MapInit() {}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK_FALSE(HasDiag(diags, codes::UnusedClass, entityClass));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, "Spawn"));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, "Precache"));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, "MapInit"));
}

TEST_CASE("UnusedSymbolChecker - base class specific lifecycle methods")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.ignoredBaseClasses = {"ScriptBasePlayerWeaponEntity", "ScriptBasePlayerAmmoEntity"};
    rules.unusedRules.lifecycleMethods = {"Spawn"};
    rules.unusedRules.baseClassLifecycleMethods["ScriptBasePlayerWeaponEntity"] = {"PrimaryAttack", "Reload"};

    const std::string weaponClass = angel_lsp::test::GenerateRandomSymbolName("Wep");
    const std::string ammoClass = angel_lsp::test::GenerateRandomSymbolName("Ammo");

    const std::string code = "class " + weaponClass +
                             " : ScriptBasePlayerWeaponEntity {\n"
                             "    private void PrimaryAttack() {}\n"
                             "}\n"
                             "class " +
                             ammoClass +
                             " : ScriptBasePlayerAmmoEntity {\n"
                             "    private void SecondaryAttack() {}\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, "PrimaryAttack")); // Weapon's PrimaryAttack exempted
    // Ammo's SecondaryAttack is NOT in baseClassLifecycleMethods for ammo, so it emits unused
    CHECK(HasDiag(diags, codes::UnusedMethod, "SecondaryAttack"));
}

TEST_CASE("UnusedSymbolChecker - base class member override exemption")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;

    const std::string baseCls = angel_lsp::test::GenerateRandomSymbolName("BaseEntity");
    const std::string derivedCls = angel_lsp::test::GenerateRandomSymbolName("DerivedEntity");

    const std::string code = "class " + baseCls +
                             " {\n"
                             "    void BaseMethod() {}\n"
                             "}\n"
                             "class " +
                             derivedCls + " : " + baseCls +
                             " {\n"
                             "    private void BaseMethod() {}\n"
                             "}\n"
                             "void Exec() { " +
                             derivedCls + " d; }\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, "BaseMethod"));
}

TEST_CASE("UnusedSymbolChecker - targeted string reflection registration with valid namespace")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.stringReflectionCallees = {{"g_CustomEntityFuncs", {"RegisterCustomEntity"}, {0}}};

    const std::string entityClass = angel_lsp::test::GenerateRandomSymbolName("K98K_CLIP");

    const std::string code = "namespace INS2_K98K {\n"
                             "    class " +
                             entityClass +
                             " {\n"
                             "        void DoStuff() {}\n"
                             "    }\n"
                             "}\n"
                             "void RegisterWeapons() {\n"
                             "    g_CustomEntityFuncs.RegisterCustomEntity(\"INS2_K98K::" +
                             entityClass +
                             "\", \"ammo\");\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK_FALSE(HasDiag(diags, codes::UnusedClass, entityClass));
}

TEST_CASE("UnusedSymbolChecker - string reflection with non-existent namespace does not exempt")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.stringReflectionCallees = {{"g_CustomEntityFuncs", {"RegisterCustomEntity"}, {0}}};

    const std::string entityClass = angel_lsp::test::GenerateRandomSymbolName("FakeClip");

    const std::string code = "namespace RealNS {\n"
                             "    class " +
                             entityClass +
                             " {\n"
                             "    }\n"
                             "}\n"
                             "void RegisterWeapons() {\n"
                             "    g_CustomEntityFuncs.RegisterCustomEntity(\"NonExistentNS::" +
                             entityClass +
                             "\", \"ammo\");\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK(HasDiag(diags, codes::UnusedClass, entityClass));
}

TEST_CASE("UnusedSymbolChecker - unrelated string literal does not exempt unused class")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.stringReflectionCallees = {{"g_CustomEntityFuncs", {"RegisterCustomEntity"}, {0}}};

    const std::string deadClass = angel_lsp::test::GenerateRandomSymbolName("DeadClass");

    const std::string code = "class " + deadClass +
                             " {\n"
                             "}\n"
                             "void LogMessage() {\n"
                             "    string msg = \"" +
                             deadClass +
                             "\";\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK(HasDiag(diags, codes::UnusedClass, deadClass));
}

TEST_CASE("UnusedSymbolChecker - string concatenation reflection")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.stringReflectionCallees = {{"g_CustomEntityFuncs", {"RegisterCustomEntity"}, {0}}};

    const std::string entityClass = angel_lsp::test::GenerateRandomSymbolName("ConcatClip");

    const std::string code = "namespace WeaponNS {\n"
                             "    class " +
                             entityClass +
                             " {\n"
                             "    }\n"
                             "}\n"
                             "void RegisterWeapons() {\n"
                             "    g_CustomEntityFuncs.RegisterCustomEntity(\"WeaponNS::\" + \"" +
                             entityClass +
                             "\", \"ammo\");\n"
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK_FALSE(HasDiag(diags, codes::UnusedClass, entityClass));
}

TEST_CASE("UnusedSymbolChecker - predefined files are ignored")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;

    const std::string unusedVar = angel_lsp::test::GenerateRandomSymbolName("g_predVar");
    const std::string unusedCls = angel_lsp::test::GenerateRandomSymbolName("PredClass");

    const std::string code = "int " + unusedVar +
                             " = 1;\n"
                             "class " +
                             unusedCls + " {}\n";

    auto diags = AnalyzeSnippet(code, &rules, "file:///sven.as.predefined");
    CHECK_FALSE(HasDiag(diags, codes::UnusedGlobalVariable, unusedVar));
    CHECK_FALSE(HasDiag(diags, codes::UnusedClass, unusedCls));
}

TEST_CASE("UnusedSymbolChecker - functions inside namespace, property accessors, and customizable entry points")
{
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("WeaponUtils");
    const std::string unusedNsFunc = angel_lsp::test::GenerateRandomSymbolName("UnusedHelper");
    const std::string usedNsFunc = angel_lsp::test::GenerateRandomSymbolName("UsedHelper");
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("ASCredit");
    const std::string propName = angel_lsp::test::GenerateRandomSymbolName("Params");
    const std::string entryFunc = angel_lsp::test::GenerateRandomSymbolName("CustomEntry");
    const std::string patternPrefix = angel_lsp::test::GenerateRandomSymbolName("HookPrefix");
    const std::string regexEntryFunc = patternPrefix + "_" + angel_lsp::test::GenerateRandomSymbolName("Callback");

    const std::string code = "namespace " + nsName +
                             " {\n"
                             "    void " +
                             unusedNsFunc +
                             "() {}\n"
                             "    void " +
                             usedNsFunc +
                             "() {}\n"
                             "}\n"
                             "class " +
                             className +
                             " {\n"
                             "    private int get_" +
                             propName +
                             "() const { return 1; }\n"
                             "}\n"
                             "void " +
                             regexEntryFunc +
                             "() {}\n"
                             "void " +
                             entryFunc +
                             "() {\n"
                             "    " +
                             nsName + "::" + usedNsFunc +
                             "();\n"
                             "    " +
                             className +
                             " c;\n"
                             "    int x = c." +
                             propName +
                             ";\n"
                             "    x = x + 1;\n"
                             "}\n";

    // Without custom exclusions, unreferenced global entry points are reported as unused
    angel_lsp::config::DiagnosticsConfig bareDiagCfg;
    bareDiagCfg.reportUnusedFunctions = true;
    auto bareDiags = AnalyzeSnippet(code, nullptr, "file:///test.as", &bareDiagCfg);
    CHECK(HasDiag(bareDiags, codes::UnusedFunction, entryFunc));
    CHECK(HasDiag(bareDiags, codes::UnusedFunction, regexEntryFunc));

    // With customizable ignoredUnusedFunctions and ignoredUnusedFunctionPatterns, they are exempted
    angel_lsp::config::DiagnosticsConfig customDiagCfg;
    customDiagCfg.reportUnusedFunctions = true;
    customDiagCfg.ignoredUnusedFunctions.insert(entryFunc);
    customDiagCfg.ignoredUnusedFunctionPatterns.push_back("^" + patternPrefix + "_.*");
    auto diags = AnalyzeSnippet(code, nullptr, "file:///test.as", &customDiagCfg);
    CHECK(HasDiag(diags, codes::UnusedFunction, unusedNsFunc));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, usedNsFunc));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, entryFunc));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, regexEntryFunc));
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, "get_" + propName));

    angel_lsp::config::DiagnosticsConfig disabledDiagCfg;
    disabledDiagCfg.reportUnusedFunctions = false;
    disabledDiagCfg.reportUnusedFunctionsExplicit = true;
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    auto diagsDisabled = AnalyzeSnippet(code, &rules, "file:///test.as", &disabledDiagCfg);
    CHECK_FALSE(HasDiag(diagsDisabled, codes::UnusedFunction, unusedNsFunc));
}

TEST_CASE("UnusedSymbolChecker - Global enum constants are not reported as unused global variables")
{
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;

    const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("WeaponAnim");
    const std::string valA = angel_lsp::test::GenerateRandomSymbolName("Reload");
    const std::string valB = angel_lsp::test::GenerateRandomSymbolName("Shoot");

    const std::string code = "enum " + enumName +
                             " {\n"
                             "    " +
                             valA +
                             ",\n"
                             "    " +
                             valB +
                             "\n"
                             "};\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK_FALSE(HasDiag(diags, codes::UnusedGlobalVariable, valA));
    CHECK_FALSE(HasDiag(diags, codes::UnusedGlobalVariable, valB));
}

TEST_CASE("UnusedSymbolChecker - Call initialized globals with ignoreUnusedGlobalsWithCallInitializer")
{
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("g_reg");
    const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("RegistryObj");

    const std::string code =
        "class " + typeName + " { " + typeName + "(string a) {} }\n" + typeName + " " + varName + "(\"init\");\n";

    // Case A: default / false -> reports unused global
    angel_lsp::config::EngineRuleConfig rulesDisabled;
    rulesDisabled.unusedRules.enabled = true;
    rulesDisabled.unusedRules.ignoreUnusedGlobalsWithCallInitializer = false;
    auto diagsDisabled = AnalyzeSnippet(code, &rulesDisabled);
    CHECK(HasDiag(diagsDisabled, codes::UnusedGlobalVariable, varName));

    // Case B: enabled -> exempts call initialized global
    angel_lsp::config::EngineRuleConfig rulesEnabled;
    rulesEnabled.unusedRules.enabled = true;
    rulesEnabled.unusedRules.ignoreUnusedGlobalsWithCallInitializer = true;
    auto diagsEnabled = AnalyzeSnippet(code, &rulesEnabled);
    CHECK_FALSE(HasDiag(diagsEnabled, codes::UnusedGlobalVariable, varName));
}

TEST_CASE("UnusedSymbolChecker - Global variable exempt types via unusedGlobalExemptTypes")
{
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("g_cmd");
    const std::string exemptType = "CClientCommand";

    const std::string code = "class " + exemptType + " {}\n" + exemptType + " " + varName + ";\n";

    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.ignoreUnusedGlobalsWithCallInitializer = false;
    rules.unusedRules.unusedGlobalExemptTypes.push_back(exemptType);

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK_FALSE(HasDiag(diags, codes::UnusedGlobalVariable, varName));
}

TEST_CASE("UnusedSymbolChecker - Global unused variable with reportUnusedGlobalVariables setting")
{
    const std::string unusedVar = angel_lsp::test::GenerateRandomSymbolName("g_unusedVar");
    const std::string code = "int " + unusedVar + ";\n";

    // Case 1: reportUnusedGlobalVariables = true via DiagnosticsConfig -> emits W117
    angel_lsp::config::DiagnosticsConfig diagEnabled;
    diagEnabled.reportUnusedGlobalVariables = true;
    diagEnabled.reportUnusedGlobalVariablesExplicit = true;
    auto diagsEnabled = AnalyzeSnippet(code, nullptr, "file:///test.as", &diagEnabled);
    CHECK(HasDiag(diagsEnabled, codes::UnusedGlobalVariable, unusedVar));

    // Case 2: reportUnusedGlobalVariables = false via DiagnosticsConfig -> does not emit W117
    angel_lsp::config::DiagnosticsConfig diagDisabled;
    diagDisabled.reportUnusedGlobalVariables = false;
    diagDisabled.reportUnusedGlobalVariablesExplicit = true;
    auto diagsDisabled = AnalyzeSnippet(code, nullptr, "file:///test.as", &diagDisabled);
    CHECK_FALSE(HasDiag(diagsDisabled, codes::UnusedGlobalVariable, unusedVar));
}

TEST_CASE("UnusedSymbolChecker - Multi-variable declarator does not self-reference")
{
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("g_firstVar");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("g_secondVar");
    const std::string code = "int " + varA + " = 1, " + varB + " = 2;\n";

    angel_lsp::config::DiagnosticsConfig diagConfig;
    diagConfig.reportUnusedGlobalVariables = true;
    diagConfig.reportUnusedGlobalVariablesExplicit = true;
    auto diags = AnalyzeSnippet(code, nullptr, "file:///test.as", &diagConfig);
    CHECK(HasDiag(diags, codes::UnusedGlobalVariable, varA));
    CHECK(HasDiag(diags, codes::UnusedGlobalVariable, varB));
}

TEST_CASE("UnusedSymbolChecker - reportUnusedClasses via DiagnosticsConfig")
{
    const std::string deadClass = angel_lsp::test::GenerateRandomSymbolName("DeadClass");
    const std::string code = "class " + deadClass + " {}\n";

    // Enabled: emits W109
    angel_lsp::config::DiagnosticsConfig diagEnabled;
    diagEnabled.reportUnusedClasses = true;
    diagEnabled.reportUnusedClassesExplicit = true;
    auto diagsEnabled = AnalyzeSnippet(code, nullptr, "file:///test.as", &diagEnabled);
    CHECK(HasDiag(diagsEnabled, codes::UnusedClass, deadClass));

    // Disabled: does not emit W109
    angel_lsp::config::DiagnosticsConfig diagDisabled;
    diagDisabled.reportUnusedClasses = false;
    diagDisabled.reportUnusedClassesExplicit = true;
    auto diagsDisabled = AnalyzeSnippet(code, nullptr, "file:///test.as", &diagDisabled);
    CHECK_FALSE(HasDiag(diagsDisabled, codes::UnusedClass, deadClass));
}

TEST_CASE("UnusedSymbolChecker - reportUnusedMembers and exemptions via DiagnosticsConfig")
{
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("EntityHandler");
    const std::string deadField = angel_lsp::test::GenerateRandomSymbolName("m_deadField");
    const std::string deadPrivateMethod = angel_lsp::test::GenerateRandomSymbolName("DeadHelper");

    const std::string code = "class " + clsName +
                             "\n"
                             "{\n"
                             "    " +
                             clsName +
                             "() {}\n"
                             "    ~" +
                             clsName +
                             "() {}\n"
                             "    int " +
                             deadField +
                             ";\n"
                             "    private void " +
                             deadPrivateMethod +
                             "() {}\n"
                             "    void opAssign(const " +
                             clsName +
                             "& in other) {}\n"
                             "    int get_Value() { return 1; }\n"
                             "}\n"
                             "void UseClass()\n"
                             "{\n"
                             "    " +
                             clsName +
                             " inst;\n"
                             "    int v = inst.Value;\n"
                             "}\n";

    angel_lsp::config::DiagnosticsConfig diagEnabled;
    diagEnabled.reportUnusedMembers = true;
    diagEnabled.reportUnusedMembersExplicit = true;
    auto diags = AnalyzeSnippet(code, nullptr, "file:///test.as", &diagEnabled);

    CHECK(HasDiag(diags, codes::UnusedField, deadField));
    CHECK(HasDiag(diags, codes::UnusedMethod, deadPrivateMethod));
    // Exemptions must not be reported
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, clsName));
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, "~" + clsName));
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, "opAssign"));
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, "get_Value"));
}

TEST_CASE("UnusedSymbolChecker - Reports unused class and unused public/default method")
{
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("ASCredit");
    const std::string unusedMethod = angel_lsp::test::GenerateRandomSymbolName("classUnused");

    const std::string code = "final class " + clsName +
                             "\n"
                             "{\n"
                             "    void " +
                             unusedMethod +
                             "() {}\n"
                             "}\n";

    angel_lsp::config::DiagnosticsConfig diag;
    diag.reportUnusedClasses = true;
    diag.reportUnusedClassesExplicit = true;
    diag.reportUnusedMembers = true;
    diag.reportUnusedMembersExplicit = true;

    auto diags = AnalyzeSnippet(code, nullptr, "file:///test.as", &diag);
    CHECK(HasDiag(diags, codes::UnusedClass, clsName));
    CHECK(HasDiag(diags, codes::UnusedMethod, unusedMethod));
}

TEST_CASE("UnusedSymbolChecker - String-literal callback references suppress unused function warning")
{
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("DestroyThrowableWeapon");
    const std::string entityName = angel_lsp::test::GenerateRandomSymbolName("env_credits");

    const std::string code = "void " + fnName +
                             "() {}\n"
                             "void " +
                             entityName +
                             "() {}\n"
                             "void Init()\n"
                             "{\n"
                             "    g_Scheduler.SetTimeout(\"" +
                             fnName +
                             "\", 1.0f);\n"
                             "    CustomEntity(\"" +
                             entityName +
                             "\", true);\n"
                             "}\n";

    angel_lsp::config::DiagnosticsConfig diag;
    diag.reportUnusedFunctions = true;
    diag.reportUnusedFunctionsExplicit = true;

    auto diags = AnalyzeSnippet(code, nullptr, "file:///test.as", &diag);
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, fnName));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, entityName));
}

TEST_CASE("UnusedSymbolChecker - Engine callback signatures exempt matching functions from W118")
{
    const std::string mapBegin = angel_lsp::test::GenerateRandomSymbolName("MapBegin");

    const std::string code =
        "void " + mapBegin + "(CBaseEntity@ pActivator, CBaseEntity@ pCaller, USE_TYPE useType, float value) {}\n";

    angel_lsp::config::EngineRuleConfig engineRules;
    engineRules.unusedRules.engineCallbackSignatures = {{"CBaseEntity@", "CBaseEntity@", "USE_TYPE", "float"}};

    angel_lsp::config::DiagnosticsConfig diag;
    diag.reportUnusedFunctions = true;
    diag.reportUnusedFunctionsExplicit = true;

    auto diags = AnalyzeSnippet(code, &engineRules, "file:///test.as", &diag);
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, mapBegin));
}

TEST_CASE("UnusedSymbolChecker - Engine-agnostic defaults without rules.json")
{
    const std::string entityClass = angel_lsp::test::GenerateRandomSymbolName("DefaultEntity");
    const std::string unusedMethod = angel_lsp::test::GenerateRandomSymbolName("DeadMethod");
    const std::string unusedGlobal = angel_lsp::test::GenerateRandomSymbolName("DeadGlobal");

    const std::string code = "int " + unusedGlobal +
                             ";\n"
                             "class " +
                             entityClass +
                             " {\n"
                             "    void Spawn() {}\n"
                             "    void Precache() {}\n"
                             "    void " +
                             unusedMethod +
                             "() {}\n"
                             "}\n"
                             "void PluginInit() {}\n"
                             "void main() {}\n"
                             "void UseIt() {\n"
                             "    " +
                             entityClass +
                             " inst;\n"
                             "}\n";

    // 1. Without rules.json (engine-agnostic default): nothing is hardcoded, so main and PluginInit are reported when
    // functions/members enabled
    angel_lsp::config::DiagnosticsConfig diag;
    diag.reportUnusedFunctions = true;
    diag.reportUnusedMembers = true;
    auto diags = AnalyzeSnippet(code, nullptr, "file:///test.as", &diag);
    CHECK(HasDiag(diags, codes::UnusedFunction, "main"));
    CHECK(HasDiag(diags, codes::UnusedFunction, "PluginInit"));
    CHECK(HasDiag(diags, codes::UnusedMethod, "Spawn"));
    CHECK(HasDiag(diags, codes::UnusedMethod, "Precache"));
    CHECK(HasDiag(diags, codes::UnusedMethod, unusedMethod));
    CHECK(HasDiag(diags, codes::UnusedGlobalVariable, unusedGlobal));

    // 1b. Configured via ignoredUnusedFunctions in diagnostics: main is exempt
    diag.ignoredUnusedFunctions.insert("main");
    auto diagsIgnored = AnalyzeSnippet(code, nullptr, "file:///test.as", &diag);
    CHECK_FALSE(HasDiag(diagsIgnored, codes::UnusedFunction, "main"));
    CHECK(HasDiag(diagsIgnored, codes::UnusedFunction, "PluginInit"));
    diag.ignoredUnusedFunctions.clear();

    // 2. With Sven Co-op engine rules: Spawn, Precache, PluginInit, main are exempt
    angel_lsp::config::EngineRuleConfig engineRules;
    engineRules.unusedRules.enabled = true;
    engineRules.unusedRules.lifecycleMethods = {"Spawn", "Precache"};
    engineRules.unusedRules.ignoredGlobalFunctions = {"PluginInit", "main"};
    auto diagsEngine = AnalyzeSnippet(code, &engineRules, "file:///test.as", &diag);
    CHECK_FALSE(HasDiag(diagsEngine, codes::UnusedFunction, "PluginInit"));
    CHECK_FALSE(HasDiag(diagsEngine, codes::UnusedFunction, "main"));
    CHECK_FALSE(HasDiag(diagsEngine, codes::UnusedMethod, "Spawn"));
    CHECK_FALSE(HasDiag(diagsEngine, codes::UnusedMethod, "Precache"));
    CHECK(HasDiag(diagsEngine, codes::UnusedMethod, unusedMethod));
    CHECK(HasDiag(diagsEngine, codes::UnusedGlobalVariable, unusedGlobal));
}

TEST_CASE("UnusedSymbolChecker - ignoredUnusedFunctions in DiagnosticsConfig suppresses globals, classes, and methods")
{
    const std::string unusedFunc = angel_lsp::test::GenerateRandomSymbolName("DeadFunc");
    const std::string unusedMethod = angel_lsp::test::GenerateRandomSymbolName("DeadMethod");
    const std::string unusedGlobal = angel_lsp::test::GenerateRandomSymbolName("DeadGlobal");
    const std::string unusedClass = angel_lsp::test::GenerateRandomSymbolName("DeadClass");
    const std::string usedClass = angel_lsp::test::GenerateRandomSymbolName("LiveClass");

    const std::string code = "int " + unusedGlobal + ";\n" + "class " + unusedClass + " {}\n" + "class " + usedClass +
                             " { void " + unusedMethod + "() {} }\n" + "void " + unusedFunc + "() {}\n" +
                             "void Entry() { " + usedClass + " inst; }\n";

    // Without ignore list: all 4 are reported when enabled
    angel_lsp::config::DiagnosticsConfig diag;
    diag.reportUnusedFunctions = true;
    diag.reportUnusedMembers = true;
    auto diags = AnalyzeSnippet(code, nullptr, "file:///test.as", &diag);
    CHECK(HasDiag(diags, codes::UnusedFunction, unusedFunc));
    CHECK(HasDiag(diags, codes::UnusedMethod, unusedMethod));
    CHECK(HasDiag(diags, codes::UnusedGlobalVariable, unusedGlobal));
    CHECK(HasDiag(diags, codes::UnusedClass, unusedClass));

    // With ignore list: all 4 are suppressed
    diag.ignoredUnusedFunctions.insert(unusedFunc);
    diag.ignoredUnusedFunctions.insert(unusedMethod);
    diag.ignoredUnusedFunctions.insert(unusedGlobal);
    diag.ignoredUnusedFunctions.insert(unusedClass);

    auto diagsIgnored = AnalyzeSnippet(code, nullptr, "file:///test.as", &diag);
    CHECK_FALSE(HasDiag(diagsIgnored, codes::UnusedFunction, unusedFunc));
    CHECK_FALSE(HasDiag(diagsIgnored, codes::UnusedMethod, unusedMethod));
    CHECK_FALSE(HasDiag(diagsIgnored, codes::UnusedGlobalVariable, unusedGlobal));
    CHECK_FALSE(HasDiag(diagsIgnored, codes::UnusedClass, unusedClass));
}

TEST_CASE("UnusedSymbolChecker - baseClassProxyMap suppresses unused warnings for overridden base methods")
{
    const std::string baseName = angel_lsp::test::GenerateRandomSymbolName("MockBase");
    const std::string proxyName = angel_lsp::test::GenerateRandomSymbolName("ScriptProxy");
    const std::string derivedName = angel_lsp::test::GenerateRandomSymbolName("CustomEntity");
    const std::string overrideMethod = angel_lsp::test::GenerateRandomSymbolName("ActionMethod");
    const std::string deadMethod = angel_lsp::test::GenerateRandomSymbolName("DeadMethod");

    const std::string predefCode = "class " + baseName + " { void " + overrideMethod + "() {} }\n";
    const std::string code = "class " + proxyName + " {}\n" + "class " + derivedName + " : " + proxyName + " {\n" +
                             "    void " + overrideMethod + "() {}\n" + "    void " + deadMethod + "() {}\n" + "}\n" +
                             "void Entry() { " + derivedName + " inst; }\n";

    // 1. Without proxy map: both methods are flagged as unused
    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    auto diagsNoRules = AnalyzeSnippetWithPredefined(code, &rules, predefCode);
    CHECK(HasDiag(diagsNoRules, codes::UnusedMethod, overrideMethod));
    CHECK(HasDiag(diagsNoRules, codes::UnusedMethod, deadMethod));

    // 2. With baseClassProxyMap linking proxyName to baseName: overrideMethod is exempt, deadMethod is still flagged
    rules.unusedRules.baseClassProxyMap[proxyName] = {baseName};
    auto diagsWithRules = AnalyzeSnippetWithPredefined(code, &rules, predefCode);
    CHECK_FALSE(HasDiag(diagsWithRules, codes::UnusedMethod, overrideMethod));
    CHECK(HasDiag(diagsWithRules, codes::UnusedMethod, deadMethod));
}

TEST_CASE("UnusedSymbolChecker - baseClassDelegateProperties discovers base methods via property")
{
    const std::string nativeClass = angel_lsp::test::GenerateRandomSymbolName("NativeEntity");
    const std::string scriptBridge = angel_lsp::test::GenerateRandomSymbolName("ScriptBridge");
    const std::string derivedClass = angel_lsp::test::GenerateRandomSymbolName("MyWeapon");
    const std::string tickMethod = angel_lsp::test::GenerateRandomSymbolName("OnTick");
    const std::string deadMethod = angel_lsp::test::GenerateRandomSymbolName("UnusedLocalMethod");

    const std::string predefCode = "class " + nativeClass + " { void " + tickMethod + "() {} }\n";
    const std::string code = "class " + scriptBridge + " {\n" + "    " + nativeClass +
                             "@ BaseClass { get const { return null; } }\n" + "}\n" + "class " + derivedClass + " : " +
                             scriptBridge + " {\n" + "    void " + tickMethod + "() {}\n" + "    void " + deadMethod +
                             "() {}\n" + "}\n" + "void Main() { " + derivedClass + " w; }\n";

    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.baseClassDelegateProperties = {"BaseClass"};

    auto diags = AnalyzeSnippetWithPredefined(code, &rules, predefCode);
    CHECK_FALSE(HasDiag(diags, codes::UnusedMethod, tickMethod));
    CHECK(HasDiag(diags, codes::UnusedMethod, deadMethod));
}

TEST_CASE("UnusedSymbolChecker - BaseClass method access does not mask truly unreferenced methods")
{
    const std::string baseClass = angel_lsp::test::GenerateRandomSymbolName("ParentClass");
    const std::string derivedClass = angel_lsp::test::GenerateRandomSymbolName("ChildClass");
    const std::string deadMethod = angel_lsp::test::GenerateRandomSymbolName("OrphanMethod");

    // deadMethod is called via BaseClass.<deadMethod>() inside its own body, but Base does not have it,
    // so it should not be considered an external reference
    const std::string code = "class " + baseClass + " {}\n" + "class " + derivedClass + " : " + baseClass + " {\n" +
                             "    " + baseClass + "@ BaseClass { get const { return null; } }\n" + "    void " +
                             deadMethod + "() {\n" + "        BaseClass." + deadMethod + "();\n" + "    }\n" + "}\n" +
                             "void Entry() { " + derivedClass + " c; }\n";

    angel_lsp::config::EngineRuleConfig rules;
    rules.unusedRules.enabled = true;
    rules.unusedRules.baseClassDelegateProperties = {"BaseClass"};

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK(HasDiag(diags, codes::UnusedMethod, deadMethod));
}

TEST_CASE("UnusedSymbolChecker - Default configuration disables unused functions and members until enabled or via Sven "
          "rules")
{
    const std::string unusedFunc = angel_lsp::test::GenerateRandomSymbolName("DeadFunc");
    const std::string unusedMethod = angel_lsp::test::GenerateRandomSymbolName("DeadMethod");
    const std::string unusedGlobal = angel_lsp::test::GenerateRandomSymbolName("DeadGlobal");
    const std::string usedClass = angel_lsp::test::GenerateRandomSymbolName("LiveClass");

    const std::string code = "int " + unusedGlobal + ";\n" + "class " + usedClass + " { void " + unusedMethod +
                             "() {} }\n" + "void " + unusedFunc + "() {}\n" + "void Entry() { " + usedClass +
                             " inst; }\n";

    // 1. Pure default DiagnosticsConfig: unused global is reported, but functions and members are suppressed
    angel_lsp::config::DiagnosticsConfig defaultDiag;
    auto diagsDefault = AnalyzeSnippet(code, nullptr, "file:///test.as", &defaultDiag);
    CHECK(HasDiag(diagsDefault, codes::UnusedGlobalVariable, unusedGlobal));
    CHECK_FALSE(HasDiag(diagsDefault, codes::UnusedFunction, unusedFunc));
    CHECK_FALSE(HasDiag(diagsDefault, codes::UnusedMethod, unusedMethod));

    // 2. With Sven Co-op engine rules active: checkFunctions and checkMembers are true, so all are reported
    angel_lsp::config::EngineRuleConfig svenRules;
    svenRules.unusedRules.enabled = true;
    svenRules.unusedRules.checkFunctions = true;
    svenRules.unusedRules.checkMembers = true;
    auto diagsSven = AnalyzeSnippet(code, &svenRules, "file:///test.as", &defaultDiag);
    CHECK(HasDiag(diagsSven, codes::UnusedGlobalVariable, unusedGlobal));
    CHECK(HasDiag(diagsSven, codes::UnusedFunction, unusedFunc));
    CHECK(HasDiag(diagsSven, codes::UnusedMethod, unusedMethod));
}
