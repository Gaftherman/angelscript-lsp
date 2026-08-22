#include <iostream>
#include <string>
#include <doctest/doctest.h>

#include "features/rename/RenameHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct MultiFileRenameEnv
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::unordered_set<std::string> predefinedUris;
        std::unordered_map<std::string, std::string> sources;
        std::unordered_map<std::string, TSTree *> trees;

        void AddFile(const std::string &uri, const std::string &code, bool isPredefined = false)
        {
            sources[uri] = code;
            TSTree *tree = parser.Parse(code);
            trees[uri] = tree;
            symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(code, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
            if (isPredefined)
            {
                predefinedUris.insert(uri);
            }
        }

        ~MultiFileRenameEnv()
        {
            for (auto &[uri, tree] : trees)
            {
                if (tree)
                {
                    ts_tree_delete(tree);
                }
            }
        }

        std::optional<lsp::PrepareRenameResult> PrepareAt(const std::string &uri, uint32_t line, uint32_t character)
        {
            PrepareRenameRequest req{
                uri,
                sources[uri],
                trees[uri],
                lsp::Position{ line, character },
                symbolTable,
                scopeIndex,
                predefinedUris
            };
            return PrepareRename(req);
        }

        std::optional<lsp::WorkspaceEdit> RenameAt(const std::string &uri, uint32_t line, uint32_t character, const std::string &newName)
        {
            RenameRequest req{
                uri,
                sources[uri],
                trees[uri],
                lsp::Position{ line, character },
                newName,
                symbolTable,
                scopeIndex,
                predefinedUris
            };
            return Rename(req);
        }
    };

    struct RenameTestEnv
    {
        MultiFileRenameEnv multiEnv;
        std::string uri = "file:///test.as";

        RenameTestEnv(const std::string &code)
        {
            multiEnv.AddFile(uri, code);
        }

        std::optional<lsp::PrepareRenameResult> PrepareAt(uint32_t line, uint32_t character)
        {
            return multiEnv.PrepareAt(uri, line, character);
        }

        std::optional<lsp::WorkspaceEdit> RenameAt(uint32_t line, uint32_t character, const std::string &newName)
        {
            return multiEnv.RenameAt(uri, line, character, newName);
        }
    };
}

TEST_CASE("RenameHandler - PrepareRename Validations and Rejections")
{
    std::string code =
        "class Player {\n"
        "    int score = 0;\n"
        "    void AddScore(int delta) {\n"
        "        score += delta;\n"
        "        return;\n"
        "    }\n"
        "}\n";

    RenameTestEnv env(code);

    SUBCASE("Valid identifiers produce PrepareRenamePlaceholder")
    {
        // Cursor on class name 'Player' at line 0, col 7
        auto prepClass = env.PrepareAt(0, 7);
        REQUIRE(prepClass.has_value());
        REQUIRE(std::holds_alternative<lsp::PrepareRenamePlaceholder>(*prepClass));
        auto placeholder = std::get<lsp::PrepareRenamePlaceholder>(*prepClass);
        CHECK(placeholder.placeholder == "Player");
        CHECK(placeholder.range.start.line == 0);
        CHECK(placeholder.range.start.character == 6);
        CHECK(placeholder.range.end.character == 12);

        // Cursor on field 'score' at line 1, col 9
        auto prepField = env.PrepareAt(1, 9);
        REQUIRE(prepField.has_value());
        REQUIRE(std::holds_alternative<lsp::PrepareRenamePlaceholder>(*prepField));
        CHECK(std::get<lsp::PrepareRenamePlaceholder>(*prepField).placeholder == "score");

        // Cursor on method 'AddScore' at line 2, col 10
        auto prepMethod = env.PrepareAt(2, 10);
        REQUIRE(prepMethod.has_value());
        CHECK(std::get<lsp::PrepareRenamePlaceholder>(*prepMethod).placeholder == "AddScore");

        // Cursor on parameter 'delta' at line 2, col 23
        auto prepParam = env.PrepareAt(2, 23);
        REQUIRE(prepParam.has_value());
        CHECK(std::get<lsp::PrepareRenamePlaceholder>(*prepParam).placeholder == "delta");
    }

    SUBCASE("Keywords and primitive types are rejected by PrepareRename")
    {
        // Keyword 'class' at line 0, col 2
        CHECK(!env.PrepareAt(0, 2).has_value());

        // Primitive type 'int' at line 1, col 5
        CHECK(!env.PrepareAt(1, 5).has_value());

        // Keyword 'void' at line 2, col 5
        CHECK(!env.PrepareAt(2, 5).has_value());

        // Keyword 'return' at line 4, col 9
        CHECK(!env.PrepareAt(4, 9).has_value());
    }

    SUBCASE("Whitespace and punctuation are rejected")
    {
        // Empty line or whitespace
        CHECK(!env.PrepareAt(0, 0).has_value());
        // Semicolon
        CHECK(!env.PrepareAt(1, 17).has_value());
    }
}

TEST_CASE("RenameHandler - Predefined Symbols Protection")
{
    MultiFileRenameEnv env;
    std::string predefinedUri = "file:///predefined.as";
    std::string predefinedCode =
        "class EngineObject {\n"
        "    void Destroy() {}\n"
        "}\n";

    std::string userUri = "file:///user.as";
    std::string userCode =
        "void main() {\n"
        "    EngineObject obj;\n"
        "    obj.Destroy();\n"
        "}\n";

    env.AddFile(predefinedUri, predefinedCode, true);
    env.AddFile(userUri, userCode, false);

    SUBCASE("PrepareRename rejects symbols in predefined files")
    {
        // Cursor on EngineObject inside predefined file
        CHECK(!env.PrepareAt(predefinedUri, 0, 7).has_value());

        // Cursor on EngineObject usage inside user file
        CHECK(!env.PrepareAt(userUri, 1, 6).has_value());

        // Cursor on Destroy method call inside user file
        CHECK(!env.PrepareAt(userUri, 2, 9).has_value());
    }

    SUBCASE("Rename execution rejects symbols in predefined files")
    {
        CHECK(!env.RenameAt(userUri, 1, 6, "NewObject").has_value());
        CHECK(!env.RenameAt(userUri, 2, 9, "Kill").has_value());
    }
}

TEST_CASE("RenameHandler - Local Variable Rename & Shadowing Isolation")
{
    std::string code =
        "void Process() {\n"
        "    int counter = 0;\n"
        "    if (true) {\n"
        "        int counter = 99;\n"
        "        counter += 1;\n"
        "    }\n"
        "    counter += 10;\n"
        "}\n";

    RenameTestEnv env(code);

    SUBCASE("Renaming outer counter modifies only outer declaration and outer usages")
    {
        auto edit = env.RenameAt(1, 10, "outerCount");
        REQUIRE(edit.has_value());
        REQUIRE(edit->changes.has_value());
        auto &changesMap = *edit->changes;
        REQUIRE(changesMap.size() == 1);

        auto it = changesMap.find(lsp::DocumentUri::parse(env.uri));
        REQUIRE(it != changesMap.end());
        const auto &edits = it->second;

        // Expect 2 edits: line 1 (decl) and line 6 (usage)
        REQUIRE(edits.size() == 2);
        CHECK(edits[0].range.start.line == 1);
        CHECK(edits[0].newText == "outerCount");
        CHECK(edits[1].range.start.line == 6);
        CHECK(edits[1].newText == "outerCount");
    }

    SUBCASE("Renaming inner counter modifies only inner declaration and inner usages")
    {
        auto edit = env.RenameAt(3, 14, "innerCount");
        REQUIRE(edit.has_value());
        REQUIRE(edit->changes.has_value());
        auto &changesMap = *edit->changes;
        REQUIRE(changesMap.size() == 1);

        auto it = changesMap.find(lsp::DocumentUri::parse(env.uri));
        REQUIRE(it != changesMap.end());
        const auto &edits = it->second;

        // Expect 2 edits: line 3 (decl) and line 4 (usage)
        REQUIRE(edits.size() == 2);
        CHECK(edits[0].range.start.line == 3);
        CHECK(edits[0].newText == "innerCount");
        CHECK(edits[1].range.start.line == 4);
        CHECK(edits[1].newText == "innerCount");
    }

    SUBCASE("Invalid new names are rejected by Rename")
    {
        // Empty name
        CHECK(!env.RenameAt(1, 10, "").has_value());
        // Starts with digit
        CHECK(!env.RenameAt(1, 10, "123invalid").has_value());
        // Contains invalid character
        CHECK(!env.RenameAt(1, 10, "invalid-name").has_value());
        // Reserved keyword
        CHECK(!env.RenameAt(1, 10, "class").has_value());
        CHECK(!env.RenameAt(1, 10, "return").has_value());
        // Primitive type name
        CHECK(!env.RenameAt(1, 10, "float").has_value());
    }
}

TEST_CASE("RenameHandler - Class Member Rename")
{
    std::string code =
        "class Player {\n"
        "    int speed = 5;\n"
        "    void Move() {\n"
        "        speed += 1;\n"
        "        this.speed = 10;\n"
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.speed = 20;\n"
        "}\n";

    RenameTestEnv env(code);

    // Rename 'speed' to 'velocity'
    auto edit = env.RenameAt(1, 9, "velocity");
    REQUIRE(edit.has_value());
    REQUIRE(edit->changes.has_value());

    auto it = edit->changes->find(lsp::DocumentUri::parse(env.uri));
    REQUIRE(it != edit->changes->end());
    const auto &edits = it->second;

    // Line 1 (decl), Line 3 (unqualified), Line 4 (this.speed), Line 9 (p.speed)
    REQUIRE(edits.size() == 4);
    CHECK(edits[0].range.start.line == 1);
    CHECK(edits[0].newText == "velocity");
    CHECK(edits[1].range.start.line == 3);
    CHECK(edits[1].newText == "velocity");
    CHECK(edits[2].range.start.line == 4);
    CHECK(edits[2].newText == "velocity");
    CHECK(edits[3].range.start.line == 9);
    CHECK(edits[3].newText == "velocity");
}

TEST_CASE("RenameHandler - Multi-File Global Symbol Rename WorkspaceEdit")
{
    MultiFileRenameEnv env;
    std::string fileA = "file:///utils.as";
    std::string codeA =
        "void HelperLog(string msg) {\n"
        "}\n";

    std::string fileB = "file:///app.as";
    std::string codeB =
        "void main() {\n"
        "    HelperLog(\"Starting app\");\n"
        "    HelperLog(\"Exiting app\");\n"
        "}\n";

    env.AddFile(fileA, codeA);
    env.AddFile(fileB, codeB);

    // Rename HelperLog to LogMessage from call site in app.as at line 1, col 5
    auto edit = env.RenameAt(fileB, 1, 5, "LogMessage");
    REQUIRE(edit.has_value());
    REQUIRE(edit->changes.has_value());

    const auto &changesMap = *edit->changes;
    REQUIRE(changesMap.size() == 2);

    auto itA = changesMap.find(lsp::DocumentUri::parse(fileA));
    REQUIRE(itA != changesMap.end());
    const auto &editsA = itA->second;
    REQUIRE(editsA.size() == 1);
    CHECK(editsA[0].range.start.line == 0);
    CHECK(editsA[0].newText == "LogMessage");

    auto itB = changesMap.find(lsp::DocumentUri::parse(fileB));
    REQUIRE(itB != changesMap.end());
    const auto &editsB = itB->second;
    REQUIRE(editsB.size() == 2);
    CHECK(editsB[0].range.start.line == 1);
    CHECK(editsB[0].newText == "LogMessage");
    CHECK(editsB[1].range.start.line == 2);
    CHECK(editsB[1].newText == "LogMessage");
}

TEST_CASE("RenameHandler - Namespace Function Rename")
{
    std::string code =
        "namespace Game {\n"
        "    void Spawn() {}\n" // line 1
        "    void Init() {\n"
        "        Spawn();\n"    // line 3
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Game::Spawn();\n"  // line 7
        "}\n";

    RenameTestEnv env(code);

    // Rename 'Spawn' to 'CreateEntity'
    auto edit = env.RenameAt(1, 9, "CreateEntity");
    REQUIRE(edit.has_value());
    REQUIRE(edit->changes.has_value());

    auto it = edit->changes->find(lsp::DocumentUri::parse(env.uri));
    REQUIRE(it != edit->changes->end());
    const auto &edits = it->second;

    REQUIRE(edits.size() == 3);
    CHECK(edits[0].range.start.line == 1);
    CHECK(edits[0].newText == "CreateEntity");
    CHECK(edits[1].range.start.line == 3);
    CHECK(edits[1].newText == "CreateEntity");
    CHECK(edits[2].range.start.line == 7);
    CHECK(edits[2].newText == "CreateEntity");
}
