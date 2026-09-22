#include <doctest/doctest.h>

#include "features/definition/DefinitionHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalyzer.h"
#include "parser/AngelScriptParser.h"

#include "helpers/TestUtils.h"
#include <random>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct SourcePos
    {
        uint32_t line = 0;
        uint32_t character = 0;
    };

    SourcePos FindPos(const std::string& source, const std::string& needle, size_t startAt = 0)
    {
        size_t pos = source.find(needle, startAt);
        if (pos == std::string::npos) return {0, 0};
        uint32_t line = 0;
        size_t lineStart = 0;
        for (size_t i = 0; i < pos; ++i)
        {
            if (source[i] == '\n')
            {
                ++line;
                lineStart = i + 1;
            }
        }
        return {line, static_cast<uint32_t>(pos - lineStart)};
    }

    struct TestEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        TestEnvironment(const std::string &code)
            : sourceCode(code)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        ~TestEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::function<std::string(const std::string &)> resolveInclude = {};

        std::optional<std::vector<lsp::Location>> DefAt(uint32_t line, uint32_t character)
        {
            DefinitionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, character } };
            req.resolveInclude = resolveInclude;
            return GetDefinition(req);
        }

        std::optional<std::vector<lsp::Location>> TypeDefAt(uint32_t line, uint32_t character)
        {
            DefinitionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, character } };
            return GetTypeDefinition(req);
        }
    };
}

TEST_CASE("DefinitionHandler - Go to Definition for Local Variable and Parameter")
{
    std::mt19937_64 rng(0x1337BEEF);
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Test");
    const std::string paramName = angel_lsp::test::GenerateIdentifier(rng, "paramA");
    const std::string localVar = angel_lsp::test::GenerateIdentifier(rng, "localB");

    std::string code = 
        "void " + fnName + "(int " + paramName + ") {\n"
        "    int " + localVar + " = 100;\n"
        "    int x = " + paramName + " + " + localVar + ";\n"
        "}\n";

    TestEnvironment env(code);

    const SourcePos paramDeclPos = FindPos(code, paramName);
    const SourcePos paramUsePos = FindPos(code, paramName, code.find("int x ="));
    auto defParam = env.DefAt(paramUsePos.line, paramUsePos.character);
    REQUIRE(defParam.has_value());
    REQUIRE(defParam->size() == 1);
    CHECK((*defParam)[0].range.start.line == paramDeclPos.line);
    CHECK((*defParam)[0].range.start.character <= paramDeclPos.character);
    CHECK((*defParam)[0].range.end.character >= paramDeclPos.character + paramName.size());

    const SourcePos varDeclPos = FindPos(code, localVar);
    const SourcePos varUsePos = FindPos(code, localVar, code.find("int x ="));
    auto defVar = env.DefAt(varUsePos.line, varUsePos.character);
    REQUIRE(defVar.has_value());
    REQUIRE(defVar->size() == 1);
    CHECK((*defVar)[0].range.start.line == varDeclPos.line);
    CHECK((*defVar)[0].range.start.character <= varDeclPos.character);
    CHECK((*defVar)[0].range.end.character >= varDeclPos.character + localVar.size());
}

TEST_CASE("DefinitionHandler - Go to Definition for Global Functions and Classes")
{
    std::mt19937_64 rng(0x1337BEF0);
    const std::string clsName = angel_lsp::test::GenerateIdentifier(rng, "TargetClass");
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "TargetFunc");

    std::string code = 
        "class " + clsName + " {}\n"
        "void " + fnName + "() {}\n"
        "void main() {\n"
        "    " + clsName + " tc;\n"
        "    " + fnName + "();\n"
        "}\n";

    TestEnvironment env(code);

    const SourcePos clsDeclPos = FindPos(code, clsName);
    const SourcePos clsUsePos = FindPos(code, clsName, code.find("void main"));
    auto defClass = env.DefAt(clsUsePos.line, clsUsePos.character);
    REQUIRE(defClass.has_value());
    REQUIRE(defClass->size() == 1);
    CHECK((*defClass)[0].range.start.line == clsDeclPos.line);
    CHECK((*defClass)[0].range.start.character <= clsDeclPos.character);
    CHECK((*defClass)[0].range.end.character >= clsDeclPos.character + clsName.size());

    const SourcePos fnDeclPos = FindPos(code, fnName);
    const SourcePos fnUsePos = FindPos(code, fnName, code.find("void main"));
    auto defFunc = env.DefAt(fnUsePos.line, fnUsePos.character);
    REQUIRE(defFunc.has_value());
    REQUIRE(defFunc->size() == 1);
    CHECK((*defFunc)[0].range.start.line == fnDeclPos.line);
    CHECK((*defFunc)[0].range.start.character <= fnDeclPos.character);
    CHECK((*defFunc)[0].range.end.character >= fnDeclPos.character + fnName.size());
}

TEST_CASE("DefinitionHandler - Invariant: Definition coordinates resilient to randomized padding and symbol names")
{
    std::mt19937_64 rng(0x1337BEF1);
    const std::string pad1 = angel_lsp::test::GenerateRandomPadding(rng);
    const std::string pad2 = angel_lsp::test::GenerateRandomPadding(rng);
    const std::string clsName = angel_lsp::test::GenerateIdentifier(rng, "Class");
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Func");
    const std::string varName = angel_lsp::test::GenerateIdentifier(rng, "var");

    std::string code =
        pad1 +
        "class " + clsName + " {}\n" +
        pad2 +
        "void " + fnName + "() {\n" +
        "    " + clsName + " " + varName + ";\n" +
        "}\n";

    TestEnvironment env(code);

    const SourcePos declPos = FindPos(code, clsName);
    const SourcePos usePos = FindPos(code, clsName, code.find("void " + fnName));

    auto defClass = env.DefAt(usePos.line, usePos.character);
    REQUIRE(defClass.has_value());
    REQUIRE(defClass->size() == 1);
    CHECK((*defClass)[0].range.start.line == declPos.line);
    CHECK((*defClass)[0].range.start.character <= declPos.character);
    CHECK((*defClass)[0].range.end.character >= declPos.character + clsName.size());
}

TEST_CASE("DefinitionHandler - Go to Definition for Class Member Access")
{
    std::string code = 
        "class Player {\n"
        "    int score;\n"
        "    void Reset() { score = 0; }\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.score = 10;\n"
        "    p.Reset();\n"
        "}\n";

    TestEnvironment env(code);

    // Go to def of 'score' in 'p.score' on line 6
    auto defField = env.DefAt(6, 7);
    REQUIRE(defField.has_value());
    REQUIRE(defField->size() == 1);
    CHECK((*defField)[0].range.start.line == 1);

    // Go to def of 'Reset' in 'p.Reset()' on line 7
    auto defMethod = env.DefAt(7, 7);
    REQUIRE(defMethod.has_value());
    REQUIRE(defMethod->size() == 1);
    CHECK((*defMethod)[0].range.start.line == 2);
}

TEST_CASE("DefinitionHandler - Go to Type Definition")
{
    std::string code = 
        "class Monster {}\n"
        "void main() {\n"
        "    Monster@ m = null;\n"
        "}\n";

    TestEnvironment env(code);

    // Go to type def of variable 'm' on line 2
    auto typeDef = env.TypeDefAt(2, 14);
    REQUIRE(typeDef.has_value());
    REQUIRE(typeDef->size() == 1);
    CHECK((*typeDef)[0].range.start.line == 0);
}

TEST_CASE("DefinitionHandler - Invalid Position Returns Nullopt")
{
    TestEnvironment env("void main() {}");
    auto def = env.DefAt(0, 12);
    CHECK(!def.has_value());
}

TEST_CASE("DefinitionHandler - Go to Definition for Class Method and Inherited Method")
{
    std::string code =
        "class Entity {\n"
        "    void TakeDamage(int dmg) {}\n" // line 1
        "}\n"
        "class Player : Entity {\n"
        "    void Attack() {\n"             // line 4
        "        TakeDamage(10);\n"          // line 5, col 9
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.Attack();\n"                  // line 10, col 7
        "}\n";

    TestEnvironment env(code);

    // Go to def of 'TakeDamage' in TakeDamage(10) on line 5
    auto defInherited = env.DefAt(5, 10);
    REQUIRE(defInherited.has_value());
    REQUIRE(defInherited->size() == 1);
    CHECK((*defInherited)[0].range.start.line == 1);

    // Go to def of 'Attack' in p.Attack() on line 10
    auto defAttack = env.DefAt(10, 7);
    REQUIRE(defAttack.has_value());
    REQUIRE(defAttack->size() == 1);
    CHECK((*defAttack)[0].range.start.line == 4);
}

TEST_CASE("DefinitionHandler - Go to Definition for Namespace Function")
{
    std::string code =
        "namespace Game {\n"
        "    void Spawn() {}\n" // line 1
        "    void Init() {\n"
        "        Spawn();\n"    // line 3, col 9
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Game::Spawn();\n"  // line 7, col 11
        "}\n";

    TestEnvironment env(code);

    // Go to def of unqualified 'Spawn()' inside namespace on line 3
    auto defInside = env.DefAt(3, 9);
    REQUIRE(defInside.has_value());
    REQUIRE(defInside->size() == 1);
    CHECK((*defInside)[0].range.start.line == 1);

    // Go to def of qualified 'Game::Spawn()' from outside on line 7
    auto defOutside = env.DefAt(7, 11);
    REQUIRE(defOutside.has_value());
    REQUIRE(defOutside->size() == 1);
    CHECK((*defOutside)[0].range.start.line == 1);
}

TEST_CASE("DefinitionHandler - Go to Definition for Include Directive")
{
    std::string code = "#include \"BuyMenu\"\nvoid main() {}\n";
    TestEnvironment env(code);
    env.resolveInclude = [](const std::string &raw) {
        if (raw == "BuyMenu")
        {
            return "file:///scripts/BuyMenu.as";
        }
        return "";
    };

    auto defs = env.DefAt(0, 12);
    REQUIRE(defs.has_value());
    REQUIRE(defs->size() == 1);
    CHECK((*defs)[0].uri.toString() == "file:///scripts/BuyMenu.as");
}

TEST_CASE("DefinitionHandler - Go to Definition for Global Property Accessor")
{
    std::string code =
        "class CModule {}\n"
        "CModule@ get_g_Module();\n"
        "void main() {\n"
        "    g_Module;\n"
        "}\n";

    TestEnvironment env(code);
    auto defs = env.DefAt(3, 6);
    REQUIRE(defs.has_value());
    REQUIRE(defs->size() == 1);
    CHECK((*defs)[0].range.start.line == 1);
}

TEST_CASE("DefinitionHandler - Go to Definition on subsequent lines after inline list pattern")
{
    const std::string stub =
        "class array<T>\n"
        "{\n"
        "    array(); // asBEHAVE_LIST_FACTORY\n"
        "    T& opIndex(uint index);\n"
        "}\n";

    const std::string &rewritten = stub;

    AngelScriptParser parser;
    SymbolCollector symbolCollector{ nullptr };
    LocalScopeCollector scopeCollector{ nullptr };
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string stubUri = "file:///as.predefined";
    TSTree *tree = parser.Parse(rewritten);
    symbolCollector.CollectSymbols(stubUri, rewritten, parser, symbolTable);
    auto rootScope = scopeCollector.CollectScopes(rewritten, parser);
    if (rootScope)
    {
        scopeIndex.SetScopeTree(stubUri, std::move(rootScope));
    }

    // Line 3: "    T& opIndex(uint index);" -> column 7 is on "opIndex"
    DefinitionRequest req{
        stubUri, rewritten, tree, symbolTable, scopeIndex,
        lsp::Position{ 3, 7 }
    };

    auto defs = GetDefinition(req);
    REQUIRE(defs.has_value());
    REQUIRE(!defs->empty());
    CHECK((*defs)[0].range.start.line == 3);

    ts_tree_delete(tree);
}

TEST_CASE("DefinitionHandler - Local and namespace variables return full declaration range")
{
    const std::string code =
        "namespace INS2_L85A2 {\n"
        "    string SPR_CAT = \"ins2/arf/\";\n"
        "}\n"
        "void main() {\n"
        "    int count = 10;\n"
        "    count++;\n"
        "    INS2_L85A2::SPR_CAT;\n"
        "}\n";

    TestEnvironment env(code);

    // Line 5: "    count++;" -> column 4 is on "count"
    auto defCount = env.DefAt(5, 4);
    REQUIRE(defCount.has_value());
    REQUIRE(!defCount->empty());
    // The range start should be at line 4, col 4 ("int count = 10;")
    CHECK((*defCount)[0].range.start.line == 4);
    CHECK((*defCount)[0].range.start.character == 4);

    // Line 6: "    INS2_L85A2::SPR_CAT;" -> column 17 is on "SPR_CAT"
    auto defSpr = env.DefAt(6, 17);
    REQUIRE(defSpr.has_value());
    REQUIRE(!defSpr->empty());
    // The range start should be at line 1, col 4 ("string SPR_CAT = \"ins2/arf/\";")
    CHECK((*defSpr)[0].range.start.line == 1);
    CHECK((*defSpr)[0].range.start.character == 4);
}

TEST_CASE("DefinitionHandler - Member access on unqualified namespaced class resolves to member definition")
{
    const std::string code =
        "namespace INS2PROP {\n"
        "    class CIns2Prop {\n"
        "        int health;\n"
        "    };\n"
        "}\n"
        "void main() {\n"
        "    CIns2Prop@ n;\n"
        "    n.health;\n"
        "}\n";

    TestEnvironment env(code);

    // Line 7: "    n.health;" -> column 7 is on "health"
    auto defs = env.DefAt(7, 7);
    REQUIRE(defs.has_value());
    REQUIRE(!defs->empty());
    CHECK((*defs)[0].range.start.line == 2);
}

TEST_CASE("DefinitionHandler - Member access on class member variable of unqualified namespaced class")
{
    const std::string code =
        "namespace INS2PROP {\n"
        "    class CIns2Prop {\n"
        "        int health;\n"
        "    };\n"
        "}\n"
        "class Weapon {\n"
        "    CIns2Prop@ n;\n"
        "    void Attack() {\n"
        "        n.health;\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);

    // Line 8: "        n.health;" -> column 11 is on "health"
    auto defs = env.DefAt(8, 11);
    REQUIRE(defs.has_value());
    REQUIRE(!defs->empty());
    CHECK((*defs)[0].range.start.line == 2);
}

TEST_CASE("DefinitionHandler - Overload-aware definition jumps to mixin origin file and line")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{ nullptr };
    LocalScopeCollector scopeCollector{ nullptr };
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string mixinUri = "file:///mixin.as";
    const std::string mixinCode =
        "mixin class WeaponBase {\n"
        "    void Deploy(string vModel, string pModel, int iAnim, string pAnim, int iBodygroup, float flDeployTime) {}\n"
        "}\n";

    const std::string weaponUri = "file:///weapon.as";
    const std::string weaponCode =
        "class weapon_ak47 : WeaponBase {\n"
        "    bool Deploy() {\n"
        "        return Deploy(\"v\", \"p\", 1, \"m16\", 0, 1.0f);\n"
        "    }\n"
        "}\n"
        "void main() {\n"
        "    weapon_ak47 w;\n"
        "    w.Deploy();\n"
        "}\n";

    // 1. Collect mixin symbols in its file
    TSTree *mixinTree = parser.Parse(mixinCode);
    symbolCollector.CollectSymbols(mixinUri, mixinCode, parser, symbolTable);

    // 2. Collect host class symbols in its file
    TSTree *weaponTree = parser.Parse(weaponCode);
    symbolCollector.CollectSymbols(weaponUri, weaponCode, parser, symbolTable);
    auto rootScope = scopeCollector.CollectScopes(weaponCode, parser);
    if (rootScope)
    {
        scopeIndex.SetScopeTree(weaponUri, std::move(rootScope));
    }

    // Line 2: "        return Deploy(\"v\", \"p\", 1, \"m16\", 0, 1.0f);" -> col 17 is on "Deploy"
    DefinitionRequest reqMixinCall{ weaponUri, weaponCode, weaponTree, symbolTable, scopeIndex, lsp::Position{ 2, 17 } };
    auto defsMixin = GetDefinition(reqMixinCall);
    REQUIRE(defsMixin.has_value());
    REQUIRE(!defsMixin->empty());
    // Must resolve to the 6-arg Deploy in the mixin file, NOT the 0-arg Deploy in weapon.as!
    CHECK((*defsMixin)[0].uri.toString() == mixinUri);
    CHECK((*defsMixin)[0].range.start.line == 1);

    // Line 7: "    w.Deploy();" -> col 7 is on "Deploy"
    DefinitionRequest reqZeroArgCall{ weaponUri, weaponCode, weaponTree, symbolTable, scopeIndex, lsp::Position{ 7, 7 } };
    auto defsZeroArg = GetDefinition(reqZeroArgCall);
    REQUIRE(defsZeroArg.has_value());
    REQUIRE(!defsZeroArg->empty());
    // Must resolve to the 0-arg Deploy in weapon.as!
    CHECK((*defsZeroArg)[0].uri.toString() == weaponUri);
    CHECK((*defsZeroArg)[0].range.start.line == 1);

    ts_tree_delete(mixinTree);
    ts_tree_delete(weaponTree);
}

TEST_CASE("Definition - F12 on declaration node returns its own definition range")
{
    TestEnvironment env(
        "mixin class WeaponMixin\n"
        "{\n"
        "    void CommonAddToPlayer() { }\n"
        "}\n");

    // Line 2: "    void CommonAddToPlayer() { }" -> col 12 is on "CommonAddToPlayer"
    auto defs = env.DefAt(2, 12);
    REQUIRE(defs.has_value());
    REQUIRE(!defs->empty());
    CHECK((*defs)[0].range.start.line == 2);
}

TEST_CASE("DefinitionHandler - Go to Type Definition for Global Property Accessor")
{
    std::string code =
        "class ModuleInfo {}\n"
        "ModuleInfo@ get_g_Module() { return null; }\n"
        "void main() {\n"
        "    g_Module;\n"
        "}\n";

    TestEnvironment env(code);

    // Line 3: "    g_Module;" -> col 5 is on "g_Module"
    auto typeDef = env.TypeDefAt(3, 5);
    REQUIRE(typeDef.has_value());
    REQUIRE(typeDef->size() == 1);
    CHECK((*typeDef)[0].range.start.line == 0);
}

TEST_CASE("DefinitionHandler - Go to Type Definition for Setter-Only Global Property Accessor")
{
    std::string code =
        "class ConfigData {}\n"
        "void set_g_Config(ConfigData@ c) {}\n"
        "void main() {\n"
        "    g_Config = null;\n"
        "}\n";

    TestEnvironment env(code);

    // Line 3: "    g_Config = null;" -> col 5 is on "g_Config"
    auto typeDef = env.TypeDefAt(3, 5);
    REQUIRE(typeDef.has_value());
    REQUIRE(typeDef->size() == 1);
    CHECK((*typeDef)[0].range.start.line == 0);
}

TEST_CASE("DefinitionHandler - Go to Type Definition for Property Accessor Resolves Scoped Type Over Disjoint Namespace")
{
    std::string code =
        "namespace Library {\n"
        "    class Config {}\n"
        "}\n"
        "namespace App {\n"
        "    class Config {}\n"
        "    Config@ get_g_Config() { return null; }\n"
        "    void main() {\n"
        "        g_Config;\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);

    // Line 7: "        g_Config;" -> col 9 is on "g_Config"
    auto typeDef = env.TypeDefAt(7, 9);
    REQUIRE(typeDef.has_value());
    REQUIRE(typeDef->size() == 1);
    // Line 4 is App::Config; line 1 is Library::Config. Must resolve to App::Config (line 4).
    CHECK((*typeDef)[0].range.start.line == 4);
}

TEST_CASE("DefinitionHandler - Go to Type Definition for Namespace-Scoped Variable")
{
    std::string code =
        "namespace Game {\n"
        "    class Player {}\n"
        "    Player g_player;\n"
        "    void main() {\n"
        "        g_player;\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);

    // Line 4: "        g_player;" -> col 9 is on "g_player"
    auto typeDef = env.TypeDefAt(4, 9);
    REQUIRE(typeDef.has_value());
    REQUIRE(typeDef->size() == 1);
    CHECK((*typeDef)[0].range.start.line == 1);
}


