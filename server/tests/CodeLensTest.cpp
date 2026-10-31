#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolCollector.h"
#include "config/EngineRuleConfig.h"
#include "features/code_lens/CodeLensHandler.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include "spdlog/fmt/fmt.h"
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
struct CodeLensFixture
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;
    std::string uri = "file:///test.as";
    std::string sourceCode;
    TSTree* tree = nullptr;

    explicit CodeLensFixture(std::string code) : sourceCode(std::move(code))
    {
        tree = parser.Parse(sourceCode);
        symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
        auto scopeRoot = scopeCollector.CollectScopes(sourceCode, parser);
        if (scopeRoot)
        {
            scopeIndex.SetScopeTree(uri, std::move(scopeRoot));
        }
    }

    ~CodeLensFixture()
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }

    void AddDocument(const std::string& fileUri, const std::string& code)
    {
        symbolCollector.CollectSymbols(fileUri, code, parser, symbolTable);
        auto scopeRoot = scopeCollector.CollectScopes(code, parser);
        if (scopeRoot)
        {
            scopeIndex.SetScopeTree(fileUri, std::move(scopeRoot));
        }
    }

    std::optional<std::vector<lsp::CodeLens>> GetLenses(const config::UnusedSymbolRules* unusedRules = nullptr,
                                                        const i18n::I18n* i18n = nullptr,
                                                        const config::ServerConfig* config = nullptr)
    {
        CodeLensRequest req{uri,     sourceCode,       tree,        symbolTable, scopeIndex,
                            nullptr, ".as.predefined", unusedRules, i18n,        config};
        return GetCodeLenses(req);
    }
};
} // namespace

TEST_CASE("CodeLens - Computes reference count for functions")
{
    CodeLensFixture fixture("void Helper() {}\n"
                            "void main()\n"
                            "{\n"
                            "    Helper();\n"
                            "    Helper();\n"
                            "}\n");

    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());
    REQUIRE(!lenses->empty());

    bool foundHelper = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 0 && lens.command.has_value())
        {
            CHECK(lens.command->title == "2 references");
            foundHelper = true;
        }
    }
    CHECK(foundHelper);
}

TEST_CASE("CodeLens - Computes implementation count for interfaces")
{
    CodeLensFixture fixture("interface IService {\n"
                            "    void Run();\n"
                            "}\n"
                            "class ServiceImpl : IService {\n"
                            "    void Run() {}\n"
                            "}\n");

    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundInterface = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 0 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 implementation");
            foundInterface = true;
        }
    }
    CHECK(foundInterface);
}

TEST_CASE("CodeLens - Computes reference count for classes")
{
    CodeLensFixture fixture("class Player {\n"
                            "    int hp;\n"
                            "}\n"
                            "void Spawn()\n"
                            "{\n"
                            "    Player p;\n"
                            "}\n");

    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundClass = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 0 && lens.command.has_value())
        {
            CHECK(lens.command->title.find("reference") != std::string::npos);
            foundClass = true;
        }
    }
    CHECK(foundClass);
}

TEST_CASE("CodeLens - Deduplicates mixin methods and aggregates references across host classes")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string mixinUri = "file:///mixin.as";
    const std::string mixinCode = "mixin class WeaponMixin {\n"
                                  "    void Deploy() {}\n"
                                  "}\n";

    const std::string hostUri = "file:///weapons.as";
    const std::string hostCode = "class Rifle : WeaponMixin {}\n"
                                 "class Pistol : WeaponMixin {}\n"
                                 "void TestCalls(Rifle@ r, Pistol@ p)\n"
                                 "{\n"
                                 "    r.Deploy();\n"
                                 "    p.Deploy();\n"
                                 "}\n";

    // Parse and collect mixin
    TSTree* mixinTree = parser.Parse(mixinCode);
    symbolCollector.CollectSymbols(mixinUri, mixinCode, parser, symbolTable);
    auto mixinScope = scopeCollector.CollectScopes(mixinCode, parser);
    if (mixinScope)
    {
        scopeIndex.SetScopeTree(mixinUri, std::move(mixinScope));
    }

    // Parse and collect hosts
    TSTree* hostTree = parser.Parse(hostCode);
    symbolCollector.CollectSymbols(hostUri, hostCode, parser, symbolTable);
    auto hostScope = scopeCollector.CollectScopes(hostCode, parser);
    if (hostScope)
    {
        scopeIndex.SetScopeTree(hostUri, std::move(hostScope));
    }

    // Resolve mixins into host classes
    symbolTable.ResolveIncludedMixins();

    // Query CodeLens on the mixin file
    CodeLensRequest req{mixinUri, mixinCode, mixinTree, symbolTable, scopeIndex};
    const auto lenses = GetCodeLenses(req);
    REQUIRE(lenses.has_value());

    // Count lenses on Deploy() line (line 1)
    size_t deployLensCount = 0;
    std::string deployTitle;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 1 && lens.command.has_value())
        {
            deployLensCount++;
            deployTitle = lens.command->title;
        }
    }

    // Must be strictly 1 CodeLens for Deploy, and aggregated reference count should be 2
    CHECK(deployLensCount == 1);
    CHECK(deployTitle == "2 references");

    if (mixinTree)
    {
        ts_tree_delete(mixinTree);
    }
    if (hostTree)
    {
        ts_tree_delete(hostTree);
    }
}

TEST_CASE("CodeLens - Provides virtual document header lens to jump to physical source")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string mixinUri = "file:///mixin.as";
    const std::string mixinCode = "mixin class WeaponMixin {\n"
                                  "    void Deploy() {}\n"
                                  "}\n";

    symbolCollector.CollectSymbols(mixinUri, mixinCode, parser, symbolTable);

    const std::string virtualUri = "angelscript-virtual://Rifle/WeaponMixin.as";
    const std::string virtualCode = "// synthesized virtual document\n";

    CodeLensRequest req{virtualUri, virtualCode, nullptr, symbolTable, scopeIndex};
    const auto lenses = GetCodeLenses(req);
    REQUIRE(lenses.has_value());
    REQUIRE(!lenses->empty());

    const auto& lens = (*lenses)[0];
    CHECK(lens.range.start.line == 0);
    REQUIRE(lens.command.has_value());
    CHECK(lens.command->command == "angelscript.openPhysicalSource");
    CHECK(lens.command->title.find("Jump to physical source in") != std::string::npos);
}

TEST_CASE("CodeLens - Provides View Mixin Expansion lens on class mixin inclusions")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string mixinUri = "file:///mixin.as";
    const std::string mixinCode = "mixin class WeaponMixin {\n"
                                  "    void Deploy() {}\n"
                                  "}\n";

    const std::string hostUri = "file:///rifle.as";
    const std::string hostCode = "class Rifle : WeaponMixin {\n"
                                 "    void Fire() {}\n"
                                 "}\n";

    TSTree* hostTree = parser.Parse(hostCode);
    symbolCollector.CollectSymbols(mixinUri, mixinCode, parser, symbolTable);
    symbolCollector.CollectSymbols(hostUri, hostCode, parser, symbolTable);
    config::ServerConfig cfg;
    cfg.features.codeLensShowMixinExpansion = true;
    CodeLensRequest req{hostUri, hostCode,         hostTree, symbolTable, scopeIndex,
                        nullptr, ".as.predefined", nullptr,  nullptr,     &cfg};
    const auto lenses = GetCodeLenses(req);
    REQUIRE(lenses.has_value());

    bool foundMixinLens = false;
    for (const auto& lens : *lenses)
    {
        if (lens.command.has_value() && lens.command->command == "angelscript.viewMixinExpansion")
        {
            foundMixinLens = true;
            CHECK(lens.command->title == "View Mixin Expansion: WeaponMixin");
        }
    }
    CHECK(foundMixinLens);

    if (hostTree)
    {
        ts_tree_delete(hostTree);
    }
}

TEST_CASE("CodeLens - Scope isolation prevents reference leakage between sibling classes")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string baseUri = "file:///base.as";
    const std::string baseCode = "class ScriptBasePlayerWeaponEntity {\n"
                                 "    void Spawn() {}\n"
                                 "}\n";

    const std::string uriA = "file:///weapon_a.as";
    const std::string codeA = "class weapon_ins2m40a1 : ScriptBasePlayerWeaponEntity {\n"
                              "    private int GetBodygroup() { return 1; }\n" // line 1
                              "    void PrimaryAttack() {\n"
                              "        GetBodygroup();\n"      // line 3
                              "        this.GetBodygroup();\n" // line 4
                              "    }\n"
                              "}\n";

    const std::string uriB = "file:///weapon_b.as";
    const std::string codeB = "class weapon_ins2ak47 : ScriptBasePlayerWeaponEntity {\n"
                              "    private int GetBodygroup() { return 2; }\n" // line 1
                              "    void PrimaryAttack() {\n"
                              "        GetBodygroup();\n" // line 3
                              "    }\n"
                              "}\n";

    TSTree* baseTree = parser.Parse(baseCode);
    TSTree* treeA = parser.Parse(codeA);
    TSTree* treeB = parser.Parse(codeB);

    symbolCollector.CollectSymbols(baseUri, baseCode, parser, symbolTable);
    symbolCollector.CollectSymbols(uriA, codeA, parser, symbolTable);
    symbolCollector.CollectSymbols(uriB, codeB, parser, symbolTable);

    auto baseScope = scopeCollector.CollectScopes(baseCode, parser);
    if (baseScope)
    {
        scopeIndex.SetScopeTree(baseUri, std::move(baseScope));
    }

    auto scopeA = scopeCollector.CollectScopes(codeA, parser);
    if (scopeA)
    {
        scopeIndex.SetScopeTree(uriA, std::move(scopeA));
    }

    auto scopeB = scopeCollector.CollectScopes(codeB, parser);
    if (scopeB)
    {
        scopeIndex.SetScopeTree(uriB, std::move(scopeB));
    }

    // Request lenses for weapon_a.as
    CodeLensRequest reqA{uriA, codeA, treeA, symbolTable, scopeIndex};
    auto lensesA = GetCodeLenses(reqA);
    REQUIRE(lensesA.has_value());

    bool foundBodygroupA = false;
    for (const auto& lens : *lensesA)
    {
        if (lens.range.start.line == 1 && lens.command.has_value())
        {
            // Must be strictly 2 references (PrimaryAttack internal calls), NOT leaking weapon_b's call
            CHECK(lens.command->title == "2 references");
            foundBodygroupA = true;
        }
    }
    CHECK(foundBodygroupA);

    // Request lenses for weapon_b.as
    CodeLensRequest reqB{uriB, codeB, treeB, symbolTable, scopeIndex};
    auto lensesB = GetCodeLenses(reqB);
    REQUIRE(lensesB.has_value());

    bool foundBodygroupB = false;
    for (const auto& lens : *lensesB)
    {
        if (lens.range.start.line == 1 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundBodygroupB = true;
        }
    }
    CHECK(foundBodygroupB);

    if (baseTree)
    {
        ts_tree_delete(baseTree);
    }
    if (treeA)
    {
        ts_tree_delete(treeA);
    }
    if (treeB)
    {
        ts_tree_delete(treeB);
    }
}

TEST_CASE("CodeLens - Method Overload Arity Isolation")
{
    CodeLensFixture fixture("class Knuckles\n"
                            "{\n"
                            "    bool Deploy() { return true; }\n"
                            "    bool Deploy(string a, string b, int c, string d, float e, bool f) { return false; }\n"
                            "    void Test()\n"
                            "    {\n"
                            "        Deploy();\n"
                            "        Deploy(\"a\", \"b\", 1, \"d\", 2.0f, true);\n"
                            "    }\n"
                            "}\n");

    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());
    REQUIRE(!lenses->empty());

    bool foundDeploy0 = false;
    bool foundDeploy6 = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 2 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundDeploy0 = true;
        }
        else if (lens.range.start.line == 3 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundDeploy6 = true;
        }
    }
    CHECK(foundDeploy0);
    CHECK(foundDeploy6);
}

TEST_CASE("CodeLens - Batched Reference Resolution Scales Beyond Arbitrary Symbol Limits")
{
    std::string code;
    code.reserve(65536);
    constexpr size_t kNumFunctions = 600;
    std::vector<std::string> funcNames;
    funcNames.reserve(kNumFunctions);

    for (size_t i = 0; i < kNumFunctions; ++i)
    {
        std::string fn = angel_lsp::test::GenerateRandomSymbolName("fn") + "_" + std::to_string(i);
        code += "void " + fn + "() {}\n";
        funcNames.push_back(std::move(fn));
    }

    code += "void Caller() {\n";
    code += "    " + funcNames.front() + "();\n";
    code += "    " + funcNames.back() + "();\n";
    code += "}\n";

    CodeLensFixture fixture(std::move(code));
    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());
    CHECK(lenses->size() == kNumFunctions + 1);

    bool verifiedFront = false;
    bool verifiedBack = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 0 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            verifiedFront = true;
        }
        else if (lens.range.start.line == kNumFunctions - 1 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            verifiedBack = true;
        }
    }
    CHECK(verifiedFront);
    CHECK(verifiedBack);
}

TEST_CASE("CodeLens - Property Accessor Reference Counting")
{
    const std::string playerClass = angel_lsp::test::GenerateRandomSymbolName("CBasePlayer");
    const std::string propName = angel_lsp::test::GenerateRandomSymbolName("BuyPoints");
    const std::string callerFn = angel_lsp::test::GenerateRandomSymbolName("GivePoints");

    std::string code = "class " + playerClass +
                       " {}\n"
                       "int get_" +
                       propName + "(" + playerClass +
                       "@ pPlayer) property { return 0; }\n"
                       "void set_" +
                       propName + "(" + playerClass +
                       "@ pPlayer, const int& in iValue) property {}\n"
                       "void " +
                       callerFn + "(" + playerClass +
                       "@ pPlayer) {\n"
                       "    " +
                       propName +
                       "[pPlayer] = 100;\n"
                       "}\n";

    CodeLensFixture fixture(std::move(code));
    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundGetterRef = false;
    bool foundSetterRef = false;

    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 1 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundGetterRef = true;
        }
        else if (lens.range.start.line == 2 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundSetterRef = true;
        }
    }

    CHECK(foundGetterRef);
    CHECK(foundSetterRef);
}

TEST_CASE("CodeLens - Cross-File Namespace Variable References")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("btscm");
    const std::string constName = angel_lsp::test::GenerateRandomSymbolName("HITGROUP_SHIELD");
    const std::string funcName = angel_lsp::test::GenerateRandomSymbolName("someFunction");

    const std::string uri1 = "file:///decl.as";
    const std::string code1 = "namespace " + nsName +
                              "\n"
                              "{\n"
                              "    const int " +
                              constName +
                              " = 15;\n"
                              "}\n";

    const std::string uri2 = "file:///usage.as";
    const std::string code2 = "namespace " + nsName +
                              "\n"
                              "{\n"
                              "    void " +
                              funcName +
                              "(int iHitGroup) {\n"
                              "        if (iHitGroup == " +
                              constName +
                              ") {}\n"
                              "    }\n"
                              "}\n";

    TSTree* tree1 = parser.Parse(code1);
    TSTree* tree2 = parser.Parse(code2);

    symbolCollector.CollectSymbols(uri1, code1, parser, symbolTable);
    symbolCollector.CollectSymbols(uri2, code2, parser, symbolTable);

    auto scope1 = scopeCollector.CollectScopes(code1, parser);
    if (scope1)
    {
        scopeIndex.SetScopeTree(uri1, std::move(scope1));
    }
    auto scope2 = scopeCollector.CollectScopes(code2, parser);
    if (scope2)
    {
        scopeIndex.SetScopeTree(uri2, std::move(scope2));
    }

    CodeLensRequest req{uri1, code1, tree1, symbolTable, scopeIndex};
    auto lenses = GetCodeLenses(req);
    REQUIRE(lenses.has_value());

    bool verified = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 2 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            verified = true;
        }
    }
    CHECK(verified);

    if (tree1)
    {
        ts_tree_delete(tree1);
    }
    if (tree2)
    {
        ts_tree_delete(tree2);
    }
}

TEST_CASE("CodeLens - Computes reference count for class methods inside same-named namespace and namespace functions")
{
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("monster_zombie");
    const std::string baseName = angel_lsp::test::GenerateRandomSymbolName("base_monster");
    const std::string methodTarget = angel_lsp::test::GenerateRandomSymbolName("CheckTraceHullAttack");
    const std::string funcTarget = angel_lsp::test::GenerateRandomSymbolName("Register");

    std::string code =
        fmt::format("class {} {{\n"
                    "    void {}(int a, int b, int c) {{}}\n"
                    "}}\n"
                    "namespace {} {{\n"
                    "    final class {} : {} {{\n"
                    "        void Attack() {{\n"
                    "            {}(70, 25, 0);\n"
                    "        }}\n"
                    "    }}\n"
                    "    void {}() {{}}\n"
                    "}}\n"
                    "void CallSite() {{\n"
                    "    {}::{}();\n"
                    "}}\n",
                    baseName, methodTarget, nsName, nsName, baseName, methodTarget, funcTarget, nsName, funcTarget);

    CodeLensFixture fixture(std::move(code));
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool methodChecked = false;
    bool funcChecked = false;
    for (const auto& lens : *lenses)
    {
        if (lens.command.has_value())
        {
            if (lens.range.start.line == 1)
            {
                CHECK(lens.command->title == "1 reference");
                methodChecked = true;
            }
            else if (lens.range.start.line == 9)
            {
                CHECK(lens.command->title == "1 reference");
                funcChecked = true;
            }
        }
    }
    CHECK(methodChecked);
    CHECK(funcChecked);
}

TEST_CASE("CodeLens - Computes reference count for class member within same class in same-named namespace")
{
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("zombie_grenadier");
    const std::string methodTarget = angel_lsp::test::GenerateRandomSymbolName("CheckTraceHullAttack");

    std::string code = fmt::format("namespace {} {{\n"
                                   "    final class {} {{\n"
                                   "        void {}(int dist, int dmg, int type) {{}}\n"
                                   "        void Attack() {{\n"
                                   "            {}(70, 25, 0);\n"
                                   "        }}\n"
                                   "    }}\n"
                                   "}}\n",
                                   nsName, nsName, methodTarget, methodTarget);

    CodeLensFixture fixture(std::move(code));
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool methodChecked = false;
    for (const auto& lens : *lenses)
    {
        if (lens.command.has_value() && lens.range.start.line == 2)
        {
            CHECK(lens.command->title == "1 reference");
            methodChecked = true;
        }
    }
    CHECK(methodChecked);
}

TEST_CASE("CodeLens - Enum Member Reference Count and Isolation from Method Calls")
{
    const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("ModeEnum");
    const std::string semiMember = angel_lsp::test::GenerateRandomSymbolName("SemiAuto");
    const std::string fullMember = angel_lsp::test::GenerateRandomSymbolName("FullAuto");
    const std::string reloadMember = angel_lsp::test::GenerateRandomSymbolName("Reload");

    std::string code = fmt::format("enum {}\n"
                                   "{{\n"
                                   "    {},\n"
                                   "    {},\n"
                                   "    {}\n"
                                   "}};\n"
                                   "class Weapon\n"
                                   "{{\n"
                                   "    void {}(int x) {{}}\n"
                                   "    void Run()\n"
                                   "    {{\n"
                                   "        int a = {}::{};\n"
                                   "        int b = {}::{};\n"
                                   "        int c = {}::{};\n"
                                   "        int d = {}::{};\n"
                                   "        {}(42);\n"
                                   "    }}\n"
                                   "}}\n",
                                   enumName, semiMember, fullMember, reloadMember, reloadMember, enumName, semiMember,
                                   enumName, semiMember, enumName, fullMember, enumName, semiMember, reloadMember);

    CodeLensFixture fixture(std::move(code));
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool checkedSemi = false;
    bool checkedFull = false;
    bool checkedReload = false;
    for (const auto& lens : *lenses)
    {
        if (!lens.command.has_value())
        {
            continue;
        }
        if (lens.range.start.line == 2)
        {
            CHECK(lens.command->title == "3 references");
            checkedSemi = true;
        }
        else if (lens.range.start.line == 3)
        {
            CHECK(lens.command->title == "1 reference");
            checkedFull = true;
        }
        else if (lens.range.start.line == 4)
        {
            // Reload enum member must NOT count the method call reload(42)
            CHECK(lens.command->title == "0 references");
            checkedReload = true;
        }
    }
    CHECK(checkedSemi);
    CHECK(checkedFull);
    CHECK(checkedReload);
}

TEST_CASE("CodeLens - Mixin class reference count across namespaces")
{
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("INS2BASE");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("AmmoBase");
    const std::string hostClassName = angel_lsp::test::GenerateRandomSymbolName("Ins2Weapon");

    std::string mixinDoc = "namespace " + nsName +
                           "\n"
                           "{\n"
                           "    mixin class " +
                           mixinName +
                           "\n"
                           "    {\n"
                           "        int maxAmmo;\n"
                           "    }\n"
                           "}\n";

    CodeLensFixture fixture(std::move(mixinDoc));

    std::string hostDoc = "class " + hostClassName + " : " + nsName + "::" + mixinName +
                          "\n"
                          "{\n"
                          "}\n";
    fixture.AddDocument("file:///weapon.as", hostDoc);

    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundMixinLens = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 2 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundMixinLens = true;
        }
    }
    CHECK(foundMixinLens);
}

TEST_CASE("CodeLens - Chained member access reference counting")
{
    const std::string entvarsClass = angel_lsp::test::GenerateRandomSymbolName("entvars_t");
    const std::string bodyProp = angel_lsp::test::GenerateRandomSymbolName("body");
    const std::string entityClass = angel_lsp::test::GenerateRandomSymbolName("CBaseEntity");
    const std::string pevProp = angel_lsp::test::GenerateRandomSymbolName("pev");

    std::string code = "class " + entvarsClass +
                       "\n"
                       "{\n"
                       "    int " +
                       bodyProp +
                       ";\n"
                       "}\n"
                       "class " +
                       entityClass +
                       "\n"
                       "{\n"
                       "    " +
                       entvarsClass + " " + pevProp +
                       ";\n"
                       "}\n"
                       "void WeaponFunc()\n"
                       "{\n"
                       "    " +
                       entityClass +
                       " self;\n"
                       "    self." +
                       pevProp + "." + bodyProp +
                       " = 1;\n"
                       "}\n";

    CodeLensFixture fixture(std::move(code));
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundBodyLens = false;
    for (const auto& lens : *lenses)
    {
        // Line 2 is `int body;`
        if (lens.range.start.line == 2 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundBodyLens = true;
        }
    }
    CHECK(foundBodyLens);
}

TEST_CASE("CodeLens - Operator overloads without call sites are suppressed")
{
    const std::string mathClass = angel_lsp::test::GenerateRandomSymbolName("VectorMath");
    const std::string normalMethod = angel_lsp::test::GenerateRandomSymbolName("NormalMethod");

    std::string code = "class " + mathClass +
                       "\n"
                       "{\n"
                       "    void opAssign(const " +
                       mathClass +
                       "& in other) {}\n"
                       "    void " +
                       normalMethod +
                       "() {}\n"
                       "}\n"
                       "void UseIt()\n"
                       "{\n"
                       "    " +
                       mathClass +
                       " v;\n"
                       "    v." +
                       normalMethod +
                       "();\n"
                       "}\n";

    CodeLensFixture fixture(std::move(code));
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundOpAssignLens = false;
    bool foundNormalLens = false;
    for (const auto& lens : *lenses)
    {
        // Line 2 is `void opAssign...`
        if (lens.range.start.line == 2 && lens.command.has_value())
        {
            foundOpAssignLens = true;
        }
        // Line 3 is `void NormalMethod...`
        if (lens.range.start.line == 3 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundNormalLens = true;
        }
    }
    CHECK_FALSE(foundOpAssignLens);
    CHECK(foundNormalLens);
}

TEST_CASE("CodeLens - Virtual property blocks report references")
{
    const std::string charClass = angel_lsp::test::GenerateRandomSymbolName("CCharacter");
    const std::string propName = angel_lsp::test::GenerateRandomSymbolName("IsHEV");

    std::string code = "class " + charClass +
                       "\n"
                       "{\n"
                       "    bool " +
                       propName +
                       " {\n"
                       "        get const { return true; }\n"
                       "    }\n"
                       "}\n"
                       "void CheckHEV(" +
                       charClass +
                       "@ c)\n"
                       "{\n"
                       "    if (c." +
                       propName +
                       ")\n"
                       "    {}\n"
                       "}\n";

    CodeLensFixture fixture(std::move(code));
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundPropLens = false;
    for (const auto& lens : *lenses)
    {
        // Line 2 is `bool <propName> {`
        if (lens.range.start.line == 2 && lens.command.has_value())
        {
            CHECK(lens.command->title == "1 reference");
            foundPropLens = true;
        }
    }
    CHECK(foundPropLens);
}

TEST_CASE("CodeLens - Class and constructor reference counting and destructor suppression")
{
    const std::string charClass = angel_lsp::test::GenerateRandomSymbolName("CCharacter");

    std::string code = "final class " + charClass +
                       "\n"
                       "{\n"
                       "    " +
                       charClass +
                       "() {}\n"
                       "    ~" +
                       charClass +
                       "() {}\n"
                       "}\n"
                       "void Init()\n"
                       "{\n"
                       "    " +
                       charClass +
                       " c;\n"
                       "    " +
                       charClass +
                       "@ handle = @c;\n"
                       "}\n";

    CodeLensFixture fixture(std::move(code));
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundClassLens = false;
    bool foundCtorLens = false;
    bool foundDtorLens = false;

    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 0 && lens.command.has_value())
        {
            // Class declaration
            foundClassLens = true;
            CHECK(lens.command->title != "0 references");
        }
        else if (lens.range.start.line == 2 && lens.command.has_value())
        {
            // Constructor
            foundCtorLens = true;
            CHECK(lens.command->title != "0 references");
        }
        else if (lens.range.start.line == 3 && lens.command.has_value())
        {
            // Destructor - should be suppressed
            foundDtorLens = true;
        }
    }
    CHECK(foundClassLens);
    CHECK(foundCtorLens);
    CHECK_FALSE(foundDtorLens);
}

TEST_CASE("CodeLens - Cross-namespace class method reference isolation")
{
    const std::string nsA = angel_lsp::test::GenerateRandomSymbolName("ASWallRechargerConfig");
    const std::string nsB = angel_lsp::test::GenerateRandomSymbolName("CustomKeyValues");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("Register");

    std::string code = "namespace " + nsA +
                       "\n"
                       "{\n"
                       "    class Config\n"
                       "    {\n"
                       "        void " +
                       methodName +
                       "() {}\n"
                       "    }\n"
                       "}\n"
                       "namespace " +
                       nsB +
                       "\n"
                       "{\n"
                       "    class Manager\n"
                       "    {\n"
                       "        void " +
                       methodName +
                       "() {}\n"
                       "    }\n"
                       "}\n"
                       "void Setup()\n"
                       "{\n"
                       "    " +
                       nsA +
                       "::Config cfg;\n"
                       "    cfg." +
                       methodName +
                       "();\n"
                       "}\n";

    CodeLensFixture fixture(std::move(code));
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundALens = false;
    bool foundBLens = false;

    for (const auto& lens : *lenses)
    {
        // Line 4 is `void Register() {}` in Config
        if (lens.range.start.line == 4 && lens.command.has_value())
        {
            foundALens = true;
            CHECK(lens.command->title == "1 reference");
        }
        // Line 11 is `void Register() {}` in Manager
        if (lens.range.start.line == 11 && lens.command.has_value())
        {
            foundBLens = true;
            CHECK(lens.command->title == "0 references");
        }
    }
    CHECK(foundALens);
    CHECK(foundBLens);
}

TEST_CASE("CodeLens - Counts string-literal callback references")
{
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("DestroyThrowableWeapon");
    const std::string code = "void " + fnName +
                             "() {}\n"
                             "void Trigger()\n"
                             "{\n"
                             "    g_Scheduler.SetTimeout(\"" +
                             fnName +
                             "\", 1.0f);\n"
                             "}\n";

    CodeLensFixture fixture(code);
    auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundLens = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 0 && lens.command.has_value())
        {
            foundLens = true;
            CHECK(lens.command->title == "1 reference");
        }
    }
    CHECK(foundLens);
}

TEST_CASE("CodeLens - Engine managed lifecycle methods and callbacks receive +1 reference")
{
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("WeaponEntity");
    const std::string code = "class " + clsName +
                             " : ScriptBasePlayerWeaponEntity\n"
                             "{\n"
                             "    void Spawn() {}\n"
                             "    void PrimaryAttack() {}\n"
                             "}\n"
                             "void MapActivate() {}\n"
                             "void MapBegin(CBaseEntity@ a, CBaseEntity@ b, USE_TYPE c, float d) {}\n";

    angel_lsp::config::UnusedSymbolRules unusedRules;
    unusedRules.ignoredBaseClasses = {"ScriptBasePlayerWeaponEntity"};
    unusedRules.lifecycleMethods = {"Spawn"};
    unusedRules.baseClassLifecycleMethods = {{"ScriptBasePlayerWeaponEntity", {"PrimaryAttack"}}};
    unusedRules.ignoredGlobalFunctions = {"MapActivate"};
    unusedRules.engineCallbackSignatures = {{"CBaseEntity@", "CBaseEntity@", "USE_TYPE", "float"}};

    CodeLensFixture fixture(code);
    auto lenses = fixture.GetLenses(&unusedRules);
    REQUIRE(lenses.has_value());

    bool foundSpawn = false;
    bool foundPrimary = false;
    bool foundActivate = false;
    bool foundMapBegin = false;

    for (const auto& lens : *lenses)
    {
        if (!lens.command.has_value())
            continue;
        // line 2: Spawn
        if (lens.range.start.line == 2)
        {
            foundSpawn = true;
            CHECK(lens.command->title == "1 reference");
        }
        // line 3: PrimaryAttack
        else if (lens.range.start.line == 3)
        {
            foundPrimary = true;
            CHECK(lens.command->title == "1 reference");
        }
        // line 5: MapActivate
        else if (lens.range.start.line == 5)
        {
            foundActivate = true;
            CHECK(lens.command->title == "1 reference");
        }
        // line 6: MapBegin
        else if (lens.range.start.line == 6)
        {
            foundMapBegin = true;
            CHECK(lens.command->title == "1 reference");
        }
    }
    CHECK(foundSpawn);
    CHECK(foundPrimary);
    CHECK(foundActivate);
    CHECK(foundMapBegin);
}

TEST_CASE("CodeLens - Reference counting for namespace variables accessed in same-namespace class")
{
    const std::string nsName = test::GenerateRandomSymbolName("INS2_AKM");
    const std::string varName = test::GenerateRandomSymbolName("RPM_AIR");
    const std::string className = test::GenerateRandomSymbolName("weapon_ins2akm");

    const std::string code = "namespace " + nsName +
                             " {\n"
                             "    const float " +
                             varName +
                             " = 600.0f;\n"
                             "    class " +
                             className +
                             " {\n"
                             "        void Fire() {\n"
                             "            float r = " +
                             varName +
                             ";\n"
                             "        }\n"
                             "    }\n"
                             "}\n";

    CodeLensFixture fixture(code);
    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundVarLens = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 1)
        {
            foundVarLens = true;
            REQUIRE(lens.command.has_value());
            CHECK(lens.command->title == "1 reference");
        }
    }
    CHECK(foundVarLens);
}

TEST_CASE("CodeLens - Spanish localized titles for references, implementations, and mixin expansion")
{
    i18n::I18n i18nEs("es");
    const std::string mixinName = test::GenerateRandomSymbolName("WeaponMixin");
    const std::string hostClass = test::GenerateRandomSymbolName("WeaponHost");
    const std::string ifaceName = test::GenerateRandomSymbolName("IWeapon");
    const std::string implClass = test::GenerateRandomSymbolName("MyWeapon");

    std::string code = "mixin class " + mixinName +
                       " {}\n"
                       "class " +
                       hostClass + " : " + mixinName +
                       " {}\n"
                       "interface " +
                       ifaceName +
                       " { void Fire(); }\n"
                       "class " +
                       implClass + " : " + ifaceName + " { void Fire() {} }\n";

    config::ServerConfig cfg;
    cfg.features.codeLensShowMixinExpansion = true;
    CodeLensFixture fixture(code);
    const auto lenses = fixture.GetLenses(nullptr, &i18nEs, &cfg);
    REQUIRE(lenses.has_value());

    bool foundMixinExpansion = false;
    bool foundImplementation = false;
    for (const auto& lens : *lenses)
    {
        if (lens.command.has_value())
        {
            if (lens.command->command == "angelscript.viewMixinExpansion")
            {
                foundMixinExpansion = true;
                CHECK(lens.command->title == "Ver expansión del mixin: " + mixinName);
            }
            else if (lens.command->title.find("implementación") != std::string::npos ||
                     lens.command->title.find("implementaciones") != std::string::npos)
            {
                foundImplementation = true;
                CHECK(lens.command->title == "1 implementación");
            }
        }
    }
    CHECK(foundMixinExpansion);
    CHECK(foundImplementation);
}

TEST_CASE("CodeLens - Separate reference counts for identical enum member names in different enums")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector(nullptr);
    LocalScopeCollector scopeCollector(nullptr);
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string enumValName = test::GenerateRandomSymbolName("Undefined");
    const std::string enum1Name = test::GenerateRandomSymbolName("Version");
    const std::string enum2Name = test::GenerateRandomSymbolName("DynamicEquipmentType");

    const std::string uri = "file:///enums.as";
    const std::string code = "enum " + enum1Name +
                             " {\n"
                             "    " +
                             enumValName +
                             " = 0\n"
                             "}\n"
                             "enum " +
                             enum2Name +
                             " {\n"
                             "    " +
                             enumValName +
                             " = 0\n"
                             "}\n"
                             "void UseVersion() {\n"
                             "    " +
                             enum1Name + " v = " + enum1Name + "::" + enumValName +
                             ";\n"
                             "}\n";

    TSTree* tree = parser.Parse(code);
    symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
    auto scope = scopeCollector.CollectScopes(code, parser);
    if (scope)
    {
        scopeIndex.SetScopeTree(uri, std::move(scope));
    }

    CodeLensRequest req{uri, code, tree, symbolTable, scopeIndex};
    auto lenses = GetCodeLenses(req);
    REQUIRE(lenses.has_value());

    bool foundEnum1Lens = false;
    bool foundEnum2Lens = false;
    for (const auto& lens : *lenses)
    {
        if (lens.range.start.line == 1 && lens.command.has_value())
        {
            foundEnum1Lens = true;
            CHECK(lens.command->title == "1 reference");
        }
        else if (lens.range.start.line == 4 && lens.command.has_value())
        {
            foundEnum2Lens = true;
            CHECK(lens.command->title == "0 references");
        }
    }
    CHECK(foundEnum1Lens);
    CHECK(foundEnum2Lens);

    if (tree)
    {
        ts_tree_delete(tree);
    }
}

TEST_CASE("CodeLens - Granular configuration and showZeroReferences")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string className = angel_lsp::test::GenerateRandomSymbolName("Weapon");
    const std::string funcName = angel_lsp::test::GenerateRandomSymbolName("Fire");
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("ammoCount");
    const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("WeaponState");

    const std::string code = "class " + className +
                             " {\n"
                             "    int " +
                             varName +
                             ";\n"
                             "    void " +
                             funcName +
                             "() {}\n"
                             "}\n"
                             "enum " +
                             enumName + " { Idle }\n";

    const std::string uri = "file:///granular_test.as";
    TSTree* tree = parser.Parse(code);
    symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
    auto scope = scopeCollector.CollectScopes(code, parser);
    if (scope)
    {
        scopeIndex.SetScopeTree(uri, std::move(scope));
    }

    {
        config::ServerConfig cfg;
        cfg.features.codeLensShowZeroReferences = true;
        CodeLensRequest req{
            .uri = uri,
            .sourceCode = code,
            .tree = tree,
            .symbolTable = symbolTable,
            .scopeIndex = scopeIndex,
            .config = &cfg,
        };
        auto lenses = GetCodeLenses(req);
        REQUIRE(lenses.has_value());
        CHECK_FALSE(lenses->empty());
    }

    {
        config::ServerConfig cfg;
        cfg.features.codeLensShowZeroReferences = false;
        CodeLensRequest req{
            .uri = uri,
            .sourceCode = code,
            .tree = tree,
            .symbolTable = symbolTable,
            .scopeIndex = scopeIndex,
            .config = &cfg,
        };
        auto lenses = GetCodeLenses(req);
        CHECK((!lenses.has_value() || lenses->empty()));
    }

    {
        config::ServerConfig cfg;
        cfg.features.codeLensShowZeroReferences = true;
        cfg.features.codeLensShowOnEnums = false;
        CodeLensRequest req{
            .uri = uri,
            .sourceCode = code,
            .tree = tree,
            .symbolTable = symbolTable,
            .scopeIndex = scopeIndex,
            .config = &cfg,
        };
        auto lenses = GetCodeLenses(req);
        REQUIRE(lenses.has_value());
        for (const auto& lens : *lenses)
        {
            CHECK(lens.range.start.line != 4);
        }
    }

    if (tree)
    {
        ts_tree_delete(tree);
    }
}

TEST_CASE("CodeLens - Zero CodeLenses emitted inside multiline license comment when file has syntax error")
{
    const std::string code = "/**\n"
                             " * Copyright (c) 2026 Mikk155 and contributors of bts_rc\n"
                             " *\n"
                             " * Permission is hereby granted, free of charge, to any person obtaining a copy\n"
                             " * of this software to use, copy, modify, merge, publish, distribute, sublicense,\n"
                             " * and/or sell copies of the Software under the following conditions:\n"
                             " *\n"
                             " * The above copyright notice and this permission notice shall be included in all\n"
                             " * copies or substantial portions of the Software.\n"
                             " *\n"
                             " * THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
                             " * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
                             " * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
                             " * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
                             " * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
                             " * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
                             " * SOFTWARE.\n"
                             " **/\n"
                             "\n"
                             "namespace item_tracker\n"
                             "{\n"
                             "    void BrokenFunction(\n"
                             "}\n";

    CodeLensFixture fixture(code);
    const auto lenses = fixture.GetLenses();
    if (lenses.has_value())
    {
        for (const auto& lens : *lenses)
        {
            CHECK_MESSAGE(lens.range.start.line > 17,
                          "Found CodeLens inside license comment at line " << lens.range.start.line);
        }
    }
}
