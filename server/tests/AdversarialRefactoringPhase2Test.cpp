#include <iostream>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <doctest/doctest.h>

#include "features/references/ReferencesHandler.h"
#include "features/rename/RenameHandler.h"
#include "config/ServerConfig.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;
using namespace angel_lsp::config;

namespace
{
    struct Phase2AdversarialEnv
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

        ~Phase2AdversarialEnv()
        {
            for (auto &[uri, tree] : trees)
            {
                if (tree)
                {
                    ts_tree_delete(tree);
                }
            }
        }

        std::optional<ReferencesResult> RefsAt(const std::string &uri, uint32_t line, uint32_t character, bool includeDecl = true)
        {
            ReferencesRequest req{
                uri,
                sources[uri],
                trees[uri],
                lsp::Position{ line, character },
                includeDecl,
                symbolTable,
                scopeIndex
            };
            return GetReferences(req);
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
}

TEST_SUITE("Phase 2 Adversarial - Find References Stress Testing")
{
    TEST_CASE("Find References - Multi-level Lexical Shadowing with Loops and Conditionals")
    {
        Phase2AdversarialEnv env;
        std::string uri = "file:///shadowing.as";
        std::string code =
            "void TestShadow() {\n"
            "    int val = 1;\n"                         // Line 1: Level 0 'val'
            "    if (val > 0) {\n"                        // Line 2: Level 0 'val' usage
            "        int val = 2;\n"                     // Line 3: Level 1 'val'
            "        for (int val = 0; val < 10; ++val) {\n" // Line 4: Level 2 'val' (decl, cond, inc)
            "            val += 1;\n"                    // Line 5: Level 2 'val' usage
            "        }\n"
            "        val += 20;\n"                       // Line 7: Level 1 'val' usage
            "    }\n"
            "    val += 100;\n"                          // Line 9: Level 0 'val' usage
            "}\n";

        env.AddFile(uri, code);

        SUBCASE("Level 0 (outermost) references")
        {
            // Cursor on outermost 'val' declaration at line 1, col 8
            auto refs = env.RefsAt(uri, 1, 8, true);
            REQUIRE(refs.has_value());
            REQUIRE(refs->size() == 3);
            CHECK((*refs)[0].range.start.line == 1);
            CHECK((*refs)[1].range.start.line == 2);
            CHECK((*refs)[2].range.start.line == 9);
        }

        SUBCASE("Level 1 (middle if block) references")
        {
            // Cursor on middle 'val' declaration at line 3, col 12
            auto refs = env.RefsAt(uri, 3, 12, true);
            REQUIRE(refs.has_value());
            REQUIRE(refs->size() == 2);
            CHECK((*refs)[0].range.start.line == 3);
            CHECK((*refs)[1].range.start.line == 7);
        }

        SUBCASE("Level 2 (innermost for loop) references")
        {
            // Cursor on for-loop 'val' declaration at line 4, col 17
            auto refs = env.RefsAt(uri, 4, 17, true);
            REQUIRE(refs.has_value());
            // Declaration (line 4 col 17), condition (line 4 col 26), increment (line 4 col 38), body (line 5 col 12)
            REQUIRE(refs->size() == 4);
            CHECK((*refs)[0].range.start.line == 4);
            CHECK((*refs)[1].range.start.line == 4);
            CHECK((*refs)[2].range.start.line == 4);
            CHECK((*refs)[3].range.start.line == 5);
        }
    }

    TEST_CASE("Find References - Multiple References on Same Line with Exact Offsets")
    {
        Phase2AdversarialEnv env;
        std::string uri = "file:///same_line.as";
        std::string code =
            "void Compute() {\n"
            "    int k = 5;\n"
            "    k = k + k * k;\n" // Line 2: 4 references to k
            "}\n";

        env.AddFile(uri, code);

        auto refs = env.RefsAt(uri, 1, 8, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 5);

        // Declaration
        CHECK((*refs)[0].range.start.line == 1);
        CHECK((*refs)[0].range.start.character == 8);
        CHECK((*refs)[0].range.end.character == 9);

        // Usages on line 2
        CHECK((*refs)[1].range.start.line == 2);
        CHECK((*refs)[1].range.start.character == 4); // LHS: k
        CHECK((*refs)[1].range.end.character == 5);

        CHECK((*refs)[2].range.start.line == 2);
        CHECK((*refs)[2].range.start.character == 8); // k + ...
        CHECK((*refs)[2].range.end.character == 9);

        CHECK((*refs)[3].range.start.line == 2);
        CHECK((*refs)[3].range.start.character == 12); // ... + k * ...
        CHECK((*refs)[3].range.end.character == 13);

        CHECK((*refs)[4].range.start.line == 2);
        CHECK((*refs)[4].range.start.character == 16); // ... * k
        CHECK((*refs)[4].range.end.character == 17);
    }

    TEST_CASE("Find References - Multi-Level Class Inheritance and Sibling Disambiguation")
    {
        Phase2AdversarialEnv env;
        std::string uri = "file:///hierarchy.as";
        std::string code =
            "class BaseNode {\n"
            "    int tag = 0;\n"
            "    void Update() { tag = 1; }\n"
            "}\n"
            "class DerivedNode : BaseNode {\n"
            "    void Process() { Update(); }\n"
            "}\n"
            "class SubDerivedNode : DerivedNode {\n"
            "    void Run() { this.Update(); }\n"
            "}\n"
            "class UnrelatedNode {\n"
            "    int tag = 99;\n"
            "    void Update() { tag = 99; }\n"
            "}\n"
            "void main() {\n"
            "    SubDerivedNode s;\n"
            "    s.Update();\n"
            "    UnrelatedNode u;\n"
            "    u.Update();\n"
            "}\n";

        env.AddFile(uri, code);

        SUBCASE("References to BaseNode::Update includes all derived classes but ignores UnrelatedNode")
        {
            // Cursor on BaseNode::Update at line 2, col 9
            auto refs = env.RefsAt(uri, 2, 9, true);
            REQUIRE(refs.has_value());
            // Expected:
            // 1. BaseNode::Update decl (line 2)
            // 2. DerivedNode::Process -> Update() (line 5)
            // 3. SubDerivedNode::Run -> this.Update() (line 8)
            // 4. main -> s.Update() (line 16)
            // UnrelatedNode::Update (lines 12, 18) MUST NOT be present!
            REQUIRE(refs->size() == 4);
            CHECK((*refs)[0].range.start.line == 2);
            CHECK((*refs)[1].range.start.line == 5);
            CHECK((*refs)[2].range.start.line == 8);
            CHECK((*refs)[3].range.start.line == 16);
        }

        SUBCASE("References to BaseNode::tag includes BaseNode usages but ignores UnrelatedNode::tag")
        {
            // Cursor on BaseNode::tag declaration at line 1, col 8
            auto refs = env.RefsAt(uri, 1, 8, true);
            REQUIRE(refs.has_value());
            // BaseNode::tag decl (line 1), tag = 1 in BaseNode::Update (line 2)
            REQUIRE(refs->size() == 2);
            CHECK((*refs)[0].range.start.line == 1);
            CHECK((*refs)[1].range.start.line == 2);
        }
    }

    TEST_CASE("Find References - Multi-File Cross Referencing with Shadowing")
    {
        Phase2AdversarialEnv env;
        std::string fileDefs = "file:///defs.as";
        std::string codeDefs =
            "int GlobalScore = 100;\n"
            "void AddScore(int amount) {\n"
            "    GlobalScore += amount;\n"
            "}\n";

        std::string fileConsumer = "file:///consumer.as";
        std::string codeConsumer =
            "void Play() {\n"
            "    AddScore(10);\n"
            "    GlobalScore += 5;\n"
            "}\n";

        std::string fileShadow = "file:///shadow.as";
        std::string codeShadow =
            "void Isolated() {\n"
            "    int GlobalScore = 0;\n" // Local shadow
            "    GlobalScore += 1;\n"
            "}\n";

        env.AddFile(fileDefs, codeDefs);
        env.AddFile(fileConsumer, codeConsumer);
        env.AddFile(fileShadow, codeShadow);

        // Find references to GlobalScore from fileDefs line 0, col 5
        auto refs = env.RefsAt(fileDefs, 0, 5, true);
        REQUIRE(refs.has_value());
        // Should contain:
        // 1. consumer.as line 2 (usage in Play) - sorted by URI alphabetically
        // 2. defs.as line 0 (decl)
        // 3. defs.as line 2 (usage in AddScore)
        // NOT shadow.as!
        REQUIRE(refs->size() == 3);
        CHECK((*refs)[0].uri.toString() == fileConsumer);
        CHECK((*refs)[0].range.start.line == 2);
        CHECK((*refs)[1].uri.toString() == fileDefs);
        CHECK((*refs)[1].range.start.line == 0);
        CHECK((*refs)[2].uri.toString() == fileDefs);
        CHECK((*refs)[2].range.start.line == 2);
    }
}

TEST_SUITE("Phase 2 Adversarial - Prepare Rename Validation & Rejections")
{
    TEST_CASE("Prepare Rename - Extensive Keyword and Primitive Type Rejection Matrix")
    {
        std::string code =
            "// A comment line\n"
            "/* Multi-line comment */\n"
            "class MyClass {\n"
            "    private int m_count = 0;\n"
            "    protected float m_speed = 1.5f;\n"
            "    void Run(bool active) {\n"
            "        if (active) {\n"
            "            for (uint i = 0; i < 10; ++i) {\n"
            "                while (m_count < 10) {\n"
            "                    return;\n"
            "                }\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "}\n";

        Phase2AdversarialEnv env;
        std::string uri = "file:///keywords.as";
        env.AddFile(uri, code);

        // 1. Comments
        CHECK(!env.PrepareAt(uri, 0, 3).has_value());  // Inside // comment
        CHECK(!env.PrepareAt(uri, 1, 5).has_value());  // Inside /* comment */

        // 2. Reserved Keywords
        CHECK(!env.PrepareAt(uri, 2, 2).has_value());  // class
        CHECK(!env.PrepareAt(uri, 3, 5).has_value());  // private
        CHECK(!env.PrepareAt(uri, 4, 6).has_value());  // protected
        CHECK(!env.PrepareAt(uri, 5, 6).has_value());  // void
        CHECK(!env.PrepareAt(uri, 6, 9).has_value());  // if
        CHECK(!env.PrepareAt(uri, 7, 13).has_value()); // for
        CHECK(!env.PrepareAt(uri, 8, 17).has_value()); // while
        CHECK(!env.PrepareAt(uri, 9, 21).has_value()); // return

        // 3. Primitive Types
        CHECK(!env.PrepareAt(uri, 3, 13).has_value()); // int
        CHECK(!env.PrepareAt(uri, 4, 15).has_value()); // float
        CHECK(!env.PrepareAt(uri, 5, 14).has_value()); // bool
        CHECK(!env.PrepareAt(uri, 7, 18).has_value()); // uint

        // 4. Whitespace & Punctuation
        CHECK(!env.PrepareAt(uri, 2, 0).has_value());  // Start of line / whitespace
        CHECK(!env.PrepareAt(uri, 3, 27).has_value()); // Semicolon
        CHECK(!env.PrepareAt(uri, 5, 25).has_value()); // Brace '{'

        // 5. Valid Identifiers MUST succeed
        auto prepClass = env.PrepareAt(uri, 2, 8); // MyClass
        REQUIRE(prepClass.has_value());
        REQUIRE(std::holds_alternative<lsp::PrepareRenamePlaceholder>(*prepClass));
        CHECK(std::get<lsp::PrepareRenamePlaceholder>(*prepClass).placeholder == "MyClass");

        auto prepField = env.PrepareAt(uri, 3, 18); // m_count
        REQUIRE(prepField.has_value());
        CHECK(std::get<lsp::PrepareRenamePlaceholder>(*prepField).placeholder == "m_count");

        auto prepMethod = env.PrepareAt(uri, 5, 10); // Run
        REQUIRE(prepMethod.has_value());
        CHECK(std::get<lsp::PrepareRenamePlaceholder>(*prepMethod).placeholder == "Run");

        auto prepParam = env.PrepareAt(uri, 5, 20); // active
        REQUIRE(prepParam.has_value());
        CHECK(std::get<lsp::PrepareRenamePlaceholder>(*prepParam).placeholder == "active");
    }

    TEST_CASE("Prepare Rename - Predefined Header Rejections")
    {
        Phase2AdversarialEnv env;
        std::string predefUri = "file:///builtin.as.predefined";
        std::string predefCode =
            "class Vector3 {\n"
            "    float x, y, z;\n"
            "    float Length() const;\n"
            "}\n";

        std::string userUri = "file:///main.as";
        std::string userCode =
            "void main() {\n"
            "    Vector3 v;\n"
            "    v.x = 1.0f;\n"
            "    float len = v.Length();\n"
            "}\n";

        env.AddFile(predefUri, predefCode, true);
        env.AddFile(userUri, userCode, false);

        // Predefined symbol usage in user file should be rejected for rename
        CHECK(!env.PrepareAt(userUri, 1, 6).has_value());  // Vector3
        CHECK(!env.PrepareAt(userUri, 2, 6).has_value());  // x
        CHECK(!env.PrepareAt(userUri, 3, 20).has_value()); // Length

        // Direct cursor in predefined file should also be rejected
        CHECK(!env.PrepareAt(predefUri, 0, 7).has_value());
        CHECK(!env.PrepareAt(predefUri, 1, 11).has_value());
        CHECK(!env.PrepareAt(predefUri, 2, 11).has_value());
    }
}

TEST_SUITE("Phase 2 Adversarial - Rename Execution & WorkspaceEdit Accuracy")
{
    TEST_CASE("Rename - WorkspaceEdit Multi-File Update & Range Precision")
    {
        Phase2AdversarialEnv env;
        std::string fileA = "file:///fileA.as";
        std::string codeA =
            "class ConfigManager {\n"
            "    int settingId = 42;\n"
            "}\n";

        std::string fileB = "file:///fileB.as";
        std::string codeB =
            "void Initialize(ConfigManager@ mgr) {\n"
            "    if (mgr !is null) {\n"
            "        mgr.settingId = 100;\n"
            "    }\n"
            "}\n";

        std::string fileC = "file:///fileC.as";
        std::string codeC =
            "void Run() {\n"
            "    ConfigManager cfg;\n"
            "    int settingId = 0;\n" // Unrelated local with same name
            "    settingId += 1;\n"
            "    cfg.settingId = settingId;\n"
            "}\n";

        env.AddFile(fileA, codeA);
        env.AddFile(fileB, codeB);
        env.AddFile(fileC, codeC);

        // Rename ConfigManager::settingId to optionId from fileA line 1, col 9
        auto edit = env.RenameAt(fileA, 1, 9, "optionId");
        REQUIRE(edit.has_value());
        REQUIRE(edit->changes.has_value());

        const auto &changes = *edit->changes;
        // File A, File B, File C should all be modified
        REQUIRE(changes.size() == 3);

        // File A: declaration
        auto itA = changes.find(lsp::DocumentUri::parse(fileA));
        REQUIRE(itA != changes.end());
        REQUIRE(itA->second.size() == 1);
        CHECK(itA->second[0].range.start.line == 1);
        CHECK(itA->second[0].range.start.character == 8);
        CHECK(itA->second[0].range.end.character == 17);
        CHECK(itA->second[0].newText == "optionId");

        // File B: member access mgr.settingId
        auto itB = changes.find(lsp::DocumentUri::parse(fileB));
        REQUIRE(itB != changes.end());
        REQUIRE(itB->second.size() == 1);
        CHECK(itB->second[0].range.start.line == 2);
        CHECK(itB->second[0].range.start.character == 12);
        CHECK(itB->second[0].range.end.character == 21);
        CHECK(itB->second[0].newText == "optionId");

        // File C: only cfg.settingId (line 4 col 8..17) should be renamed!
        // Unrelated local settingId on line 2 & 3 must NOT be renamed!
        auto itC = changes.find(lsp::DocumentUri::parse(fileC));
        REQUIRE(itC != changes.end());
        REQUIRE(itC->second.size() == 1);
        CHECK(itC->second[0].range.start.line == 4);
        CHECK(itC->second[0].range.start.character == 8);
        CHECK(itC->second[0].range.end.character == 17);
        CHECK(itC->second[0].newText == "optionId");
    }

    TEST_CASE("Rename - Rejection of Invalid Identifiers as Target New Names")
    {
        Phase2AdversarialEnv env;
        std::string uri = "file:///valid.as";
        std::string code = "void TargetFunc() {}\n";
        env.AddFile(uri, code);

        // Invalid target names:
        CHECK(!env.RenameAt(uri, 0, 7, "").has_value());
        CHECK(!env.RenameAt(uri, 0, 7, "123num").has_value());
        CHECK(!env.RenameAt(uri, 0, 7, "my-func").has_value());
        CHECK(!env.RenameAt(uri, 0, 7, "my func").has_value());
        CHECK(!env.RenameAt(uri, 0, 7, "class").has_value());
        CHECK(!env.RenameAt(uri, 0, 7, "void").has_value());
        CHECK(!env.RenameAt(uri, 0, 7, "float").has_value());
        CHECK(!env.RenameAt(uri, 0, 7, "return").has_value());
        CHECK(!env.RenameAt(uri, 0, 7, "true").has_value());

        // Valid target names:
        CHECK(env.RenameAt(uri, 0, 7, "NewValidFunc").has_value());
        CHECK(env.RenameAt(uri, 0, 7, "_private_func_123").has_value());
    }
}

TEST_SUITE("Phase 2 Adversarial - ServerConfig CLI Flags Verification")
{
    TEST_CASE("ServerConfig - Case-Insensitive Flag Variations and Robust Parsing")
    {
        const char *argv1[] = {
            "angel_lsp",
            "--enable-rename=FALSE",
            "--enable-references=0",
            "--disable-hover=false"
        };
        ServerConfig cfg1 = FromArgs(4, const_cast<char**>(argv1));
        CHECK(cfg1.features.enableRename == false);
        CHECK(cfg1.features.enableReferences == false);
        CHECK(cfg1.features.enableHover == true);

        const char *argv2[] = {
            "angel_lsp",
            "--enable-rename=Off",
            "--enable-references=NO",
            "--disable-documentsymbols"
        };
        ServerConfig cfg2 = FromArgs(4, const_cast<char**>(argv2));
        CHECK(cfg2.features.enableRename == false);
        CHECK(cfg2.features.enableReferences == false);
        CHECK(cfg2.features.enableDocumentSymbols == false);

        const char *argv3[] = {
            "angel_lsp",
            "--disable-rename",
            "--enable-rename=true",
            "--enable-references",
            "false"
        };
        ServerConfig cfg3 = FromArgs(5, const_cast<char**>(argv3));
        CHECK(cfg3.features.enableRename == true);
        CHECK(cfg3.features.enableReferences == false);
    }
}
