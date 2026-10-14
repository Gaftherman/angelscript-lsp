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
                             "}\n";

    auto diags = AnalyzeSnippet(code, &rules);
    CHECK(HasDiag(diags, codes::UnusedFunction, unusedFunc));
    CHECK_FALSE(HasDiag(diags, codes::UnusedFunction, usedFunc));
    CHECK(HasDiag(diags, codes::UnusedMethod, unusedPrivateMethod));
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
