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
