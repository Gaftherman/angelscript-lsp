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
