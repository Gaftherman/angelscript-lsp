#include <doctest/doctest.h>

#include "features/code_lens/CodeLensHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "parser/AngelScriptParser.h"

#include <string>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct CodeLensFixture
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        explicit CodeLensFixture(std::string code)
            : sourceCode(std::move(code))
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

        std::optional<std::vector<lsp::CodeLens>> GetLenses()
        {
            CodeLensRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex };
            return GetCodeLenses(req);
        }
    };
}

TEST_CASE("CodeLens - Computes reference count for functions")
{
    CodeLensFixture fixture(
        "void Helper() {}\n"
        "void main()\n"
        "{\n"
        "    Helper();\n"
        "    Helper();\n"
        "}\n"
    );

    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());
    REQUIRE(!lenses->empty());

    bool foundHelper = false;
    for (const auto &lens : *lenses)
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
    CodeLensFixture fixture(
        "interface IService {\n"
        "    void Run();\n"
        "}\n"
        "class ServiceImpl : IService {\n"
        "    void Run() {}\n"
        "}\n"
    );

    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundInterface = false;
    for (const auto &lens : *lenses)
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
    CodeLensFixture fixture(
        "class Player {\n"
        "    int hp;\n"
        "}\n"
        "void Spawn()\n"
        "{\n"
        "    Player p;\n"
        "}\n"
    );

    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());

    bool foundClass = false;
    for (const auto &lens : *lenses)
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
    SymbolCollector symbolCollector{ nullptr };
    LocalScopeCollector scopeCollector{ nullptr };
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string mixinUri = "file:///mixin.as";
    const std::string mixinCode =
        "mixin class WeaponMixin {\n"
        "    void Deploy() {}\n"
        "}\n";

    const std::string hostUri = "file:///weapons.as";
    const std::string hostCode =
        "class Rifle : WeaponMixin {}\n"
        "class Pistol : WeaponMixin {}\n"
        "void TestCalls(Rifle@ r, Pistol@ p)\n"
        "{\n"
        "    r.Deploy();\n"
        "    p.Deploy();\n"
        "}\n";

    // Parse and collect mixin
    TSTree *mixinTree = parser.Parse(mixinCode);
    symbolCollector.CollectSymbols(mixinUri, mixinCode, parser, symbolTable);
    auto mixinScope = scopeCollector.CollectScopes(mixinCode, parser);
    if (mixinScope)
    {
        scopeIndex.SetScopeTree(mixinUri, std::move(mixinScope));
    }

    // Parse and collect hosts
    TSTree *hostTree = parser.Parse(hostCode);
    symbolCollector.CollectSymbols(hostUri, hostCode, parser, symbolTable);
    auto hostScope = scopeCollector.CollectScopes(hostCode, parser);
    if (hostScope)
    {
        scopeIndex.SetScopeTree(hostUri, std::move(hostScope));
    }

    // Resolve mixins into host classes
    symbolTable.ResolveIncludedMixins();

    // Query CodeLens on the mixin file
    CodeLensRequest req{ mixinUri, mixinCode, mixinTree, symbolTable, scopeIndex };
    const auto lenses = GetCodeLenses(req);
    REQUIRE(lenses.has_value());

    // Count lenses on Deploy() line (line 1)
    size_t deployLensCount = 0;
    std::string deployTitle;
    for (const auto &lens : *lenses)
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
    SymbolCollector symbolCollector{ nullptr };
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string mixinUri = "file:///mixin.as";
    const std::string mixinCode =
        "mixin class WeaponMixin {\n"
        "    void Deploy() {}\n"
        "}\n";

    symbolCollector.CollectSymbols(mixinUri, mixinCode, parser, symbolTable);

    const std::string virtualUri = "angelscript-virtual://Rifle/WeaponMixin.as";
    const std::string virtualCode = "// synthesized virtual document\n";

    CodeLensRequest req{ virtualUri, virtualCode, nullptr, symbolTable, scopeIndex };
    const auto lenses = GetCodeLenses(req);
    REQUIRE(lenses.has_value());
    REQUIRE(!lenses->empty());

    const auto &lens = (*lenses)[0];
    CHECK(lens.range.start.line == 0);
    REQUIRE(lens.command.has_value());
    CHECK(lens.command->command == "angelscript.openPhysicalSource");
    CHECK(lens.command->title.find("Jump to physical source in") != std::string::npos);
}

TEST_CASE("CodeLens - Provides View Mixin Expansion lens on class mixin inclusions")
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{ nullptr };
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string mixinUri = "file:///mixin.as";
    const std::string mixinCode =
        "mixin class WeaponMixin {\n"
        "    void Deploy() {}\n"
        "}\n";

    const std::string hostUri = "file:///rifle.as";
    const std::string hostCode =
        "class Rifle : WeaponMixin {\n"
        "    void Fire() {}\n"
        "}\n";

    TSTree *hostTree = parser.Parse(hostCode);
    symbolCollector.CollectSymbols(mixinUri, mixinCode, parser, symbolTable);
    symbolCollector.CollectSymbols(hostUri, hostCode, parser, symbolTable);
    symbolTable.ResolveIncludedMixins();

    CodeLensRequest req{ hostUri, hostCode, hostTree, symbolTable, scopeIndex };
    const auto lenses = GetCodeLenses(req);
    REQUIRE(lenses.has_value());

    bool foundMixinLens = false;
    for (const auto &lens : *lenses)
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
    SymbolCollector symbolCollector{ nullptr };
    LocalScopeCollector scopeCollector{ nullptr };
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string baseUri = "file:///base.as";
    const std::string baseCode =
        "class ScriptBasePlayerWeaponEntity {\n"
        "    void Spawn() {}\n"
        "}\n";

    const std::string uriA = "file:///weapon_a.as";
    const std::string codeA =
        "class weapon_ins2m40a1 : ScriptBasePlayerWeaponEntity {\n"
        "    private int GetBodygroup() { return 1; }\n" // line 1
        "    void PrimaryAttack() {\n"
        "        GetBodygroup();\n"                      // line 3
        "        this.GetBodygroup();\n"                 // line 4
        "    }\n"
        "}\n";

    const std::string uriB = "file:///weapon_b.as";
    const std::string codeB =
        "class weapon_ins2ak47 : ScriptBasePlayerWeaponEntity {\n"
        "    private int GetBodygroup() { return 2; }\n" // line 1
        "    void PrimaryAttack() {\n"
        "        GetBodygroup();\n"                      // line 3
        "    }\n"
        "}\n";

    TSTree *baseTree = parser.Parse(baseCode);
    TSTree *treeA = parser.Parse(codeA);
    TSTree *treeB = parser.Parse(codeB);

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
    CodeLensRequest reqA{ uriA, codeA, treeA, symbolTable, scopeIndex };
    auto lensesA = GetCodeLenses(reqA);
    REQUIRE(lensesA.has_value());

    bool foundBodygroupA = false;
    for (const auto &lens : *lensesA)
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
    CodeLensRequest reqB{ uriB, codeB, treeB, symbolTable, scopeIndex };
    auto lensesB = GetCodeLenses(reqB);
    REQUIRE(lensesB.has_value());

    bool foundBodygroupB = false;
    for (const auto &lens : *lensesB)
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
    CodeLensFixture fixture(
        "class Knuckles\n"
        "{\n"
        "    bool Deploy() { return true; }\n"
        "    bool Deploy(string a, string b, int c, string d, float e, bool f) { return false; }\n"
        "    void Test()\n"
        "    {\n"
        "        Deploy();\n"
        "        Deploy(\"a\", \"b\", 1, \"d\", 2.0f, true);\n"
        "    }\n"
        "}\n"
    );

    const auto lenses = fixture.GetLenses();
    REQUIRE(lenses.has_value());
    REQUIRE(!lenses->empty());

    bool foundDeploy0 = false;
    bool foundDeploy6 = false;
    for (const auto &lens : *lenses)
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

