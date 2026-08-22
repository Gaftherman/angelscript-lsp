#include <doctest/doctest.h>

#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct WorkspaceSymbolTestEnv
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        SymbolTable symbolTable;

        void AddFile(const std::string &uri, const std::string &sourceCode)
        {
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
        }

        std::optional<WorkspaceSymbolResult> Search(std::string_view query, size_t maxResults = 100)
        {
            WorkspaceSymbolRequest req{ query, symbolTable, maxResults };
            return GetWorkspaceSymbols(req);
        }
    };
}

TEST_CASE("WorkspaceSymbolHandler - Exact Match Ranking")
{
    WorkspaceSymbolTestEnv env;
    std::string code =
        "class Player {}\n"
        "void PlaySound() {}\n"
        "class PlayerController {}\n"
        "int GetPlayer() { return 0; }\n";

    env.AddFile("file:///game.as", code);

    auto result = env.Search("Player");
    REQUIRE(result.has_value());
    REQUIRE(!result->empty());

    // The exact match "Player" should be ranked first
    CHECK((*result)[0].name == "Player");
    CHECK((*result)[0].kind == lsp::SymbolKind::Class);
}

TEST_CASE("WorkspaceSymbolHandler - Prefix and Substring Matching")
{
    WorkspaceSymbolTestEnv env;
    std::string code =
        "void PlaySound() {}\n"
        "void PlayMusic() {}\n"
        "void StopAudio() {}\n";

    env.AddFile("file:///audio.as", code);

    // Prefix search
    auto prefixRes = env.Search("Play");
    REQUIRE(prefixRes.has_value());
    CHECK(prefixRes->size() >= 2);
    for (const auto &sym : *prefixRes)
    {
        CHECK(sym.name.starts_with("Play"));
    }

    // Substring search
    auto subRes = env.Search("Music");
    REQUIRE(subRes.has_value());
    REQUIRE(subRes->size() == 1);
    CHECK((*subRes)[0].name == "PlayMusic");
}

TEST_CASE("WorkspaceSymbolHandler - Case Insensitive Search")
{
    WorkspaceSymbolTestEnv env;
    std::string code = "class PlayerCharacter {};\n";
    env.AddFile("file:///player.as", code);

    auto res = env.Search("playercharacter");
    REQUIRE(res.has_value());
    REQUIRE(res->size() == 1);
    CHECK((*res)[0].name == "PlayerCharacter");
}

TEST_CASE("WorkspaceSymbolHandler - Qualified Name Matching")
{
    WorkspaceSymbolTestEnv env;
    std::string code =
        "namespace Engine\n"
        "{\n"
        "    namespace Audio\n"
        "    {\n"
        "        void StreamSound() {}\n"
        "    }\n"
        "}\n";

    env.AddFile("file:///engine.as", code);

    auto res = env.Search("Audio");
    REQUIRE(res.has_value());
    REQUIRE(!res->empty());

    bool foundStreamSound = false;
    for (const auto &sym : *res)
    {
        if (sym.name == "StreamSound")
        {
            foundStreamSound = true;
        }
    }
    CHECK(foundStreamSound);
}

TEST_CASE("WorkspaceSymbolHandler - Fuzzy Subsequence Search")
{
    WorkspaceSymbolTestEnv env;
    std::string code = "class GameStateManager {};\n";
    env.AddFile("file:///state.as", code);

    auto res = env.Search("gsm");
    REQUIRE(res.has_value());
    REQUIRE(!res->empty());
    CHECK((*res)[0].name == "GameStateManager");
}

TEST_CASE("WorkspaceSymbolHandler - Empty Query Returns All Symbols")
{
    WorkspaceSymbolTestEnv env;
    std::string code =
        "int a = 1;\n"
        "int b = 2;\n"
        "int c = 3;\n";

    env.AddFile("file:///vars.as", code);

    auto res = env.Search("");
    REQUIRE(res.has_value());
    CHECK(res->size() == 3);
}

TEST_CASE("WorkspaceSymbolHandler - MaxResults Truncation")
{
    WorkspaceSymbolTestEnv env;
    std::string code =
        "int var0 = 0;\n"
        "int var1 = 1;\n"
        "int var2 = 2;\n"
        "int var3 = 3;\n"
        "int var4 = 4;\n";

    env.AddFile("file:///many.as", code);

    auto res = env.Search("var", 3);
    REQUIRE(res.has_value());
    CHECK(res->size() == 3);
}

TEST_CASE("WorkspaceSymbolHandler - Container Name and Symbol Kind Mapping")
{
    WorkspaceSymbolTestEnv env;
    std::string code =
        "class Vehicle\n"
        "{\n"
        "    int speed;\n"
        "    void Accelerate() {}\n"
        "}\n";

    env.AddFile("file:///vehicle.as", code);

    auto resSpeed = env.Search("speed");
    REQUIRE(resSpeed.has_value());
    REQUIRE(!resSpeed->empty());
    CHECK((*resSpeed)[0].name == "speed");
    CHECK((*resSpeed)[0].kind == lsp::SymbolKind::Field);
    REQUIRE((*resSpeed)[0].containerName.has_value());
    CHECK((*resSpeed)[0].containerName.value() == "Vehicle");

    auto resAcc = env.Search("Accelerate");
    REQUIRE(resAcc.has_value());
    REQUIRE(!resAcc->empty());
    CHECK((*resAcc)[0].name == "Accelerate");
    CHECK((*resAcc)[0].kind == lsp::SymbolKind::Method);
    REQUIRE((*resAcc)[0].containerName.has_value());
    CHECK((*resAcc)[0].containerName.value() == "Vehicle");
}

TEST_CASE("WorkspaceSymbolHandler - Enum Deduplication")
{
    WorkspaceSymbolTestEnv env;
    std::string code =
        "enum Difficulty\n"
        "{\n"
        "    Easy,\n"
        "    Normal,\n"
        "    Hard\n"
        "}\n";

    env.AddFile("file:///enum.as", code);

    auto res = env.Search("Easy");
    REQUIRE(res.has_value());
    // Should be deduplicated to exactly 1 result despite dual scope indexing
    CHECK(res->size() == 1);
    CHECK((*res)[0].name == "Easy");
}

TEST_CASE("WorkspaceSymbolHandler - Multi-File Workspace Symbols")
{
    WorkspaceSymbolTestEnv env;
    env.AddFile("file:///fileA.as", "class ActorA {};\n");
    env.AddFile("file:///fileB.as", "class ActorB {};\n");

    auto res = env.Search("Actor");
    REQUIRE(res.has_value());
    REQUIRE(res->size() == 2);

    bool foundA = false;
    bool foundB = false;
    for (const auto &sym : *res)
    {
        if (sym.name == "ActorA")
        {
            foundA = true;
            CHECK(sym.location.uri.toString().find("fileA.as") != std::string::npos);
        }
        if (sym.name == "ActorB")
        {
            foundB = true;
            CHECK(sym.location.uri.toString().find("fileB.as") != std::string::npos);
        }
    }
    CHECK(foundA);
    CHECK(foundB);
}
