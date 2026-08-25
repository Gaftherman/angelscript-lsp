#include <doctest/doctest.h>

#include "features/code_action/CodeActionHandler.h"
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
    /**
     * @brief In-memory test environment for code actions and refactorings.
     */
    struct RefactorTestEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        RefactorTestEnvironment(const std::string &code)
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

        ~RefactorTestEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<std::vector<lsp::CodeAction>> CodeActions(
            lsp::Range range = lsp::Range{ { 0, 0 }, { 0, 0 } },
            lsp::CodeActionContext context = lsp::CodeActionContext{})
        {
            CodeActionRequest req{ uri, sourceCode, tree, range, context, symbolTable, scopeIndex };
            return GetCodeActions(req);
        }
    };
}

TEST_CASE("Refactoring - Extract Variable: Arithmetic Expression")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int a = 1;\n"
        "    int b = 2;\n"
        "    int c = a + b * 2;\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "a + b * 2" on line 4, cols 12 to 21
    lsp::Range range{ { 4, 12 }, { 4, 21 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            CHECK(action.kind.has_value());
            CHECK(action.kind.value() == lsp::CodeActionKind::RefactorExtract);
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Edit 0: Declaration inserted before line 4
            CHECK(edits[0].range.start.line == 4);
            CHECK(edits[0].range.start.character == 0);
            CHECK(edits[0].newText.find("int newVar = a + b * 2;\n") != std::string::npos);

            // Edit 1: Replacement of expression with newVar
            CHECK(edits[1].range.start.line == 4);
            CHECK(edits[1].range.start.character == 12);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Refactoring - Extract Variable: Call Expression")
{
    std::string code =
        "class Player {}\n"
        "class Target {}\n"
        "Target@ GetTarget(Player@ p) { return null; }\n"
        "void main()\n"
        "{\n"
        "    Player@ player = Player();\n"
        "    Target@ t = GetTarget(player);\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "GetTarget(player)" on line 6, cols 16 to 33
    lsp::Range range{ { 6, 16 }, { 6, 33 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            CHECK(edits[0].newText.find("Target@ newVar = GetTarget(player);") != std::string::npos);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Refactoring - Extract Variable: Inside If Condition")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int x = 5;\n"
        "    int y = 10;\n"
        "    if (x + y > 10)\n"
        "    {\n"
        "    }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "x + y" on line 4, cols 8 to 13
    lsp::Range range{ { 4, 8 }, { 4, 13 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            CHECK(edits[0].range.start.line == 4);
            CHECK(edits[0].range.start.character == 0);
            CHECK(edits[0].newText.find("int newVar = x + y;\n") != std::string::npos);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Refactoring - Extract Variable: Auto Fallback on Untyped/Complex Expression")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    DoSomething(UnknownFunc());\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "UnknownFunc()" on line 2, cols 16 to 29
    lsp::Range range{ { 2, 16 }, { 2, 29 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            CHECK(edits[0].newText.find("auto newVar = UnknownFunc();\n") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Refactoring - Extract Variable: Rejects Assignment LHS")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int x = 0;\n"
        "    x = 10;\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "x" on line 3, cols 4 to 5 (LHS of assignment)
    lsp::Range range{ { 3, 4 }, { 3, 5 } };
    auto actions = env.CodeActions(range);

    if (actions.has_value())
    {
        for (const auto &action : *actions)
        {
            CHECK(action.title != "Extract Variable");
        }
    }
}

TEST_CASE("Refactoring - Extract Method: Simple Void Statements")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int a = 1;\n"
        "    Print(a);\n"
        "    Print(a + 1);\n"
        "    int b = 2;\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select lines 3 to 4: "Print(a);\n    Print(a + 1);"
    lsp::Range range{ { 3, 4 }, { 4, 17 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Method")
        {
            foundExtract = true;
            CHECK(action.kind.has_value());
            CHECK(action.kind.value() == lsp::CodeActionKind::RefactorExtract);
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Call site edit replaces selected statements
            CHECK(edits[0].newText.find("NewMethod(a);") != std::string::npos);

            // Method definition inserted after function
            CHECK(edits[1].newText.find("void NewMethod(int a)") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Refactoring - Extract Method: Single Return Value")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int a = 1;\n"
        "    int b = 2;\n"
        "    int sum = a + b;\n"
        "    Print(sum);\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select lines 2 to 4: "int a = 1;\n    int b = 2;\n    int sum = a + b;"
    lsp::Range range{ { 2, 4 }, { 4, 20 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Method")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Call site assigns sum
            CHECK(edits[0].newText.find("int sum = NewMethod();") != std::string::npos);

            // Method returns int
            CHECK(edits[1].newText.find("int NewMethod()") != std::string::npos);
            CHECK(edits[1].newText.find("return sum;") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Refactoring - Extract Method: Inside Class Definition")
{
    std::string code =
        "class Calculator\n"
        "{\n"
        "    void Compute()\n"
        "    {\n"
        "        int x = 10;\n"
        "        int y = 20;\n"
        "        int total = x + y;\n"
        "        Print(total);\n"
        "    }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select lines 4 to 6
    lsp::Range range{ { 4, 8 }, { 6, 26 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Method")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Call site inside class
            CHECK(edits[0].newText.find("int total = NewMethod();") != std::string::npos);

            // Method definition inserted inside class body before '}'
            CHECK(edits[1].range.start.line == 9);
            CHECK(edits[1].newText.find("int NewMethod()") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Refactoring - Generate Getters and Setters: Prefixed Field (m_speed)")
{
    std::string code =
        "class Player\n"
        "{\n"
        "    int m_speed;\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Position cursor on m_speed (line 2, col 8)
    lsp::Range range{ { 2, 8 }, { 2, 8 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundGetter = false;
    bool foundSetter = false;
    bool foundBoth = false;

    for (const auto &action : *actions)
    {
        if (action.title == "Generate Getter")
        {
            foundGetter = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.find("int get_Speed() const") != std::string::npos);
            CHECK(edits[0].newText.find("return m_speed;") != std::string::npos);
        }
        else if (action.title == "Generate Setter")
        {
            foundSetter = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.find("void set_Speed(int value)") != std::string::npos);
            CHECK(edits[0].newText.find("m_speed = value;") != std::string::npos);
        }
        else if (action.title == "Generate Getter and Setter")
        {
            foundBoth = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.find("int get_Speed() const") != std::string::npos);
            CHECK(edits[0].newText.find("void set_Speed(int value)") != std::string::npos);
        }
    }

    CHECK(foundGetter);
    CHECK(foundSetter);
    CHECK(foundBoth);
}

TEST_CASE("Refactoring - Generate Getters and Setters: Complex Object Field (string m_name)")
{
    std::string code =
        "class Player\n"
        "{\n"
        "    string m_name;\n"
        "}\n";

    RefactorTestEnvironment env(code);
    lsp::Range range{ { 2, 8 }, { 2, 8 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundSetter = false;

    for (const auto &action : *actions)
    {
        if (action.title == "Generate Setter")
        {
            foundSetter = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.find("void set_Name(const string &in value)") != std::string::npos);
            CHECK(edits[0].newText.find("m_name = value;") != std::string::npos);
        }
    }

    CHECK(foundSetter);
}

TEST_CASE("Refactoring - Generate Getters and Setters: Existing Getter Skipped")
{
    std::string code =
        "class Player\n"
        "{\n"
        "    int m_speed;\n"
        "    int get_Speed() const { return m_speed; }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    lsp::Range range{ { 2, 8 }, { 2, 8 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundGetter = false;
    bool foundSetter = false;
    bool foundBoth = false;

    for (const auto &action : *actions)
    {
        if (action.title == "Generate Getter") foundGetter = true;
        if (action.title == "Generate Setter") foundSetter = true;
        if (action.title == "Generate Getter and Setter") foundBoth = true;
    }

    CHECK(!foundGetter);
    CHECK(foundSetter);
    CHECK(!foundBoth);
}

TEST_CASE("Refactoring - Add const Qualifier: Quick Fix on Diagnostic")
{
    std::string code =
        "class Character\n"
        "{\n"
        "    void Attack() {}\n"
        "}\n"
        "void Test(const Character &in c)\n"
        "{\n"
        "    c.Attack();\n"
        "}\n";

    RefactorTestEnvironment env(code);

    lsp::Diagnostic diag;
    diag.code = lsp::String("as-err-const-method-required");
    diag.range = lsp::Range{ { 6, 6 }, { 6, 12 } }; // "Attack"
    lsp::CodeActionContext ctx;
    ctx.diagnostics = { diag };

    auto actions = env.CodeActions(diag.range, ctx);

    REQUIRE(actions.has_value());
    bool foundConstFix = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Add 'const' qualifier to method")
        {
            foundConstFix = true;
            CHECK(action.kind.has_value());
            CHECK(action.kind.value() == lsp::CodeActionKind::QuickFix);
            CHECK(action.isPreferred.value_or(false) == true);
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].range.start.line == 2);
            CHECK(edits[0].newText == " const");
        }
    }
    CHECK(foundConstFix);
}

TEST_CASE("Refactoring - Add const Qualifier: Intention on Read-Only Method")
{
    std::string code =
        "class Character\n"
        "{\n"
        "    int m_health;\n"
        "    int GetHealth()\n"
        "    {\n"
        "        return m_health;\n"
        "    }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    lsp::Range range{ { 3, 8 }, { 3, 8 } }; // on GetHealth declaration
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundConstAction = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Add 'const' qualifier to method")
        {
            foundConstAction = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].range.start.line == 3);
            CHECK(edits[0].newText == " const");
        }
    }
    CHECK(foundConstAction);
}

TEST_CASE("Refactoring - Add const Qualifier: Mutating Method Ignored")
{
    std::string code =
        "class Character\n"
        "{\n"
        "    int m_health;\n"
        "    void TakeDamage(int dmg)\n"
        "    {\n"
        "        m_health -= dmg;\n"
        "    }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    lsp::Range range{ { 3, 9 }, { 3, 9 } }; // on TakeDamage
    auto actions = env.CodeActions(range);

    if (actions.has_value())
    {
        for (const auto &action : *actions)
        {
            CHECK(action.title != "Add 'const' qualifier to method");
        }
    }
}

TEST_CASE("Refactoring - Sort and Clean Includes: Alphabetical and Grouped")
{
    std::string code =
        "#include \"utils/Math.as\"\n"
        "#include <system/Core.as>\n"
        "#include \"common/Types.as\"\n"
        "#include <engine/Audio.as>\n"
        "\n"
        "void main() {}\n";

    RefactorTestEnvironment env(code);
    lsp::Range range{ { 0, 0 }, { 3, 20 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundOrganize = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Sort and Clean #include Directives")
        {
            foundOrganize = true;
            CHECK(action.kind.has_value());
            CHECK(action.kind.value() == lsp::CodeActionKind::SourceOrganizeImports);
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].range.start.line == 0);
            CHECK(edits[0].range.end.line == 4);

            std::string expected =
                "#include <engine/Audio.as>\n"
                "#include <system/Core.as>\n"
                "\n"
                "#include \"common/Types.as\"\n"
                "#include \"utils/Math.as\"\n";

            CHECK(edits[0].newText == expected);
        }
    }
    CHECK(foundOrganize);
}

TEST_CASE("Refactoring - Sort and Clean Includes: Removes Duplicates")
{
    std::string code =
        "#include \"utils/Math.as\"\n"
        "#include \"utils/Math.as\"\n"
        "#include <system/Core.as>\n"
        "\n"
        "void main() {}\n";

    RefactorTestEnvironment env(code);
    auto actions = env.CodeActions();

    REQUIRE(actions.has_value());
    bool foundOrganize = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Sort and Clean #include Directives")
        {
            foundOrganize = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());

            std::string expected =
                "#include <system/Core.as>\n"
                "\n"
                "#include \"utils/Math.as\"\n";

            CHECK(edits[0].newText == expected);
        }
    }
    CHECK(foundOrganize);
}

TEST_CASE("Refactoring - Sort and Clean Includes: Removes Unused When Symbols Known")
{
    // Populate symbolTable with symbols in an included file
    RefactorTestEnvironment env("void dummy() {}\n");
    env.sourceCode =
        "#include \"unused.as\"\n"
        "#include \"used.as\"\n"
        "\n"
        "void main()\n"
        "{\n"
        "    UsedFunction();\n"
        "}\n";

    env.tree = env.parser.Parse(env.sourceCode);
    env.symbolCollector.CollectSymbols(env.uri, env.sourceCode, env.parser, env.symbolTable);

    // Register symbols in the included files
    Symbol usedSym;
    usedSym.name = "UsedFunction";
    usedSym.type = SymbolType::Function;
    usedSym.fileUri = "used.as";
    env.symbolTable.AddSymbol(usedSym);

    Symbol unusedSym;
    unusedSym.name = "UnusedFunction";
    unusedSym.type = SymbolType::Function;
    unusedSym.fileUri = "unused.as";
    env.symbolTable.AddSymbol(unusedSym);

    auto rootScope = env.scopeCollector.CollectScopes(env.sourceCode, env.parser);
    if (rootScope)
    {
        env.scopeIndex.SetScopeTree(env.uri, std::move(rootScope));
    }

    auto actions = env.CodeActions();
    REQUIRE(actions.has_value());
    bool foundOrganize = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Sort and Clean #include Directives")
        {
            foundOrganize = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.find("#include \"used.as\"") != std::string::npos);
            CHECK(edits[0].newText.find("#include \"unused.as\"") == std::string::npos);
        }
    }
    CHECK(foundOrganize);
}

TEST_CASE("Adversarial Refactoring - Extract Variable: Chained Call and Indexing")
{
    std::string code =
        "class Inventory { array<int> items; }\n"
        "class Player { Inventory@ GetInventory() { return null; } }\n"
        "void main()\n"
        "{\n"
        "    Player@ player = Player();\n"
        "    int ammo = player.GetInventory().items[0];\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "player.GetInventory().items[0]" on line 5, cols 15 to 44
    lsp::Range range{ { 5, 15 }, { 5, 44 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            CHECK(edits[0].range.start.line == 5);
            CHECK(edits[0].newText.find("newVar = player.GetInventory().items[0];\n") != std::string::npos);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial Refactoring - Extract Variable: Nested Binary Expressions")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int a = 1, b = 2, c = 3, d = 4;\n"
        "    int res = (a + b) * (c + d);\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "(a + b)" on line 3, cols 14 to 21
    lsp::Range range{ { 3, 14 }, { 3, 21 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            CHECK(edits[0].range.start.line == 3);
            CHECK(edits[0].newText.find("int newVar = (a + b);\n") != std::string::npos);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial Refactoring - Extract Variable: Inside For Loop Header")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    array<int> list;\n"
        "    for (uint i = 0; i < list.length(); ++i)\n"
        "    {\n"
        "    }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "list.length()" on line 3, cols 25 to 38
    lsp::Range range{ { 3, 25 }, { 3, 38 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            // Declaration inserted before the for statement (line 3)
            CHECK(edits[0].range.start.line == 3);
            CHECK(edits[0].newText.find("newVar = list.length();\n") != std::string::npos);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial Refactoring - Extract Variable: Ternary Expression")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    bool flag = true;\n"
        "    int val = flag ? 100 : 200;\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "flag ? 100 : 200" on line 3, cols 14 to 30
    lsp::Range range{ { 3, 14 }, { 3, 30 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            CHECK(edits[0].newText.find("newVar = flag ? 100 : 200;\n") != std::string::npos);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial Refactoring - Extract Method: Multiple Inputs and Multiple Outputs")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int a = 10;\n"
        "    int b = 20;\n"
        "    int c = 30;\n"
        "    int out1 = a + b;\n"
        "    int out2 = b + c;\n"
        "    Print(out1 + out2);\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select lines 5 to 6: "int out1 = a + b;\n    int out2 = b + c;"
    lsp::Range range{ { 5, 4 }, { 6, 21 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Method")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Call site: return out1, pass out2 as &out
            CHECK(edits[0].newText.find("int out1 = NewMethod(a, b, c, out2);") != std::string::npos);

            // Method definition: int NewMethod(int a, int b, int c, int &out out2)
            CHECK(edits[1].newText.find("int NewMethod(int a, int b, int c, int &out out2)") != std::string::npos);
            CHECK(edits[1].newText.find("return out1;") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial Refactoring - Extract Method: Member Variables In Class")
{
    std::string code =
        "class Entity\n"
        "{\n"
        "    int m_health;\n"
        "    int m_armor;\n"
        "    void Damage(int amount)\n"
        "    {\n"
        "        int absorbed = amount / 2;\n"
        "        m_armor -= absorbed;\n"
        "        m_health -= (amount - absorbed);\n"
        "    }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select lines 6 to 8: absorbed, m_armor, m_health
    lsp::Range range{ { 6, 8 }, { 8, 40 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Method")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Only local parameter `amount` should be in input parameters, NOT member variables `m_health`/`m_armor`
            MESSAGE("Call edit text: " << edits[0].newText);
            MESSAGE("Method def edit text: " << edits[1].newText);
            CHECK(edits[0].newText.find("NewMethod(amount);") != std::string::npos);
            CHECK(edits[1].newText.find("void NewMethod(int amount)") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial Refactoring - Generate Getters and Setters: Weird Field Names (m_pScore, _val_2, x)")
{
    std::string code =
        "class TestClass\n"
        "{\n"
        "    int m_pScore;\n"
        "    float _val_2;\n"
        "    bool x;\n"
        "}\n";

    RefactorTestEnvironment env(code);

    // 1. Test m_pScore -> PScore
    {
        lsp::Range range{ { 2, 8 }, { 2, 8 } };
        auto actions = env.CodeActions(range);
        REQUIRE(actions.has_value());
        bool foundBoth = false;
        for (const auto &act : *actions)
        {
            if (act.title == "Generate Getter and Setter")
            {
                foundBoth = true;
                REQUIRE(act.edit.has_value());
                auto changes = act.edit->changes.value();
                auto edits = changes[lsp::DocumentUri::parse(env.uri)];
                CHECK(edits[0].newText.find("int get_PScore() const") != std::string::npos);
                CHECK(edits[0].newText.find("void set_PScore(int value)") != std::string::npos);
            }
        }
        CHECK(foundBoth);
    }

    // 2. Test _val_2 -> Val_2
    {
        lsp::Range range{ { 3, 10 }, { 3, 10 } };
        auto actions = env.CodeActions(range);
        REQUIRE(actions.has_value());
        bool foundBoth = false;
        for (const auto &act : *actions)
        {
            if (act.title == "Generate Getter and Setter")
            {
                foundBoth = true;
                REQUIRE(act.edit.has_value());
                auto changes = act.edit->changes.value();
                auto edits = changes[lsp::DocumentUri::parse(env.uri)];
                CHECK(edits[0].newText.find("float get_Val_2() const") != std::string::npos);
                CHECK(edits[0].newText.find("void set_Val_2(float value)") != std::string::npos);
            }
        }
        CHECK(foundBoth);
    }

    // 3. Test x -> X
    {
        lsp::Range range{ { 4, 9 }, { 4, 9 } };
        auto actions = env.CodeActions(range);
        REQUIRE(actions.has_value());
        bool foundBoth = false;
        for (const auto &act : *actions)
        {
            if (act.title == "Generate Getter and Setter")
            {
                foundBoth = true;
                REQUIRE(act.edit.has_value());
                auto changes = act.edit->changes.value();
                auto edits = changes[lsp::DocumentUri::parse(env.uri)];
                CHECK(edits[0].newText.find("bool get_X() const") != std::string::npos);
                CHECK(edits[0].newText.find("void set_X(bool value)") != std::string::npos);
            }
        }
        CHECK(foundBoth);
    }
}

TEST_CASE("Adversarial Refactoring - Generate Getters and Setters: Existing Setter Only Generates Getter")
{
    std::string code =
        "class Player\n"
        "{\n"
        "    int m_speed;\n"
        "    void set_Speed(int val) { m_speed = val; }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    lsp::Range range{ { 2, 8 }, { 2, 8 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundGetter = false;
    bool foundSetter = false;
    bool foundBoth = false;

    for (const auto &action : *actions)
    {
        if (action.title == "Generate Getter") foundGetter = true;
        if (action.title == "Generate Setter") foundSetter = true;
        if (action.title == "Generate Getter and Setter") foundBoth = true;
    }

    CHECK(foundGetter);
    CHECK(!foundSetter);
    CHECK(!foundBoth);
}

TEST_CASE("Adversarial Refactoring - Add const: Method Calling Const vs Non-Const Helpers")
{
    std::string code =
        "class Game\n"
        "{\n"
        "    int m_state;\n"
        "    void Mutate() { m_state = 1; }\n"
        "    int ReadOnlyHelper() const { return m_state; }\n"
        "    int MethodCallingNonConst() { Mutate(); return 0; }\n"
        "    int MethodCallingConst() { return ReadOnlyHelper(); }\n"
        "}\n";

    RefactorTestEnvironment env(code);

    // MethodCallingNonConst (line 5, col 8) should NOT get 'Add const qualifier'
    {
        lsp::Range range{ { 5, 8 }, { 5, 8 } };
        auto actions = env.CodeActions(range);
        if (actions.has_value())
        {
            for (const auto &act : *actions)
            {
                CHECK(act.title != "Add 'const' qualifier to method");
            }
        }
    }

    // MethodCallingConst (line 6, col 8) SHOULD get 'Add const qualifier'
    {
        lsp::Range range{ { 6, 8 }, { 6, 8 } };
        auto actions = env.CodeActions(range);
        REQUIRE(actions.has_value());
        bool foundConst = false;
        for (const auto &act : *actions)
        {
            if (act.title == "Add 'const' qualifier to method")
            {
                foundConst = true;
            }
        }
        CHECK(foundConst);
    }
}

TEST_CASE("Adversarial Refactoring - Add const: Local Variable With Same Name Does Not Mutate Field")
{
    std::string code =
        "class Player\n"
        "{\n"
        "    int count;\n"
        "    int Calculate()\n"
        "    {\n"
        "        int count = 5;\n"
        "        count += 10;\n"
        "        return count;\n"
        "    }\n"
        "}\n";

    RefactorTestEnvironment env(code);
    lsp::Range range{ { 3, 8 }, { 3, 8 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundConst = false;
    for (const auto &act : *actions)
    {
        if (act.title == "Add 'const' qualifier to method")
        {
            foundConst = true;
        }
    }
    CHECK(foundConst);
}

TEST_CASE("Adversarial Refactoring - Sort and Clean Includes: Mixed System/Local With Unresolved Preserved")
{
    std::string code =
        "#include \"unresolved/Config.as\"\n"
        "#include <engine/Renderer.as>\n"
        "#include \"unresolved/Config.as\"\n"
        "#include <core/Math.as>\n"
        "\n"
        "void main() {}\n";

    RefactorTestEnvironment env(code);
    auto actions = env.CodeActions();

    REQUIRE(actions.has_value());
    bool foundOrganize = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Sort and Clean #include Directives")
        {
            foundOrganize = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());

            std::string expected =
                "#include <core/Math.as>\n"
                "#include <engine/Renderer.as>\n"
                "\n"
                "#include \"unresolved/Config.as\"\n";

            CHECK(edits[0].newText == expected);
        }
    }
    CHECK(foundOrganize);
}

TEST_CASE("Adversarial Refactoring - Extract Variable: Unary Minus and Cast Expression")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int speed = 50;\n"
        "    float v = -float(speed);\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select "-float(speed)" on line 3, cols 14 to 27
    lsp::Range range{ { 3, 14 }, { 3, 27 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Variable")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            CHECK(edits[0].newText.find("newVar = -float(speed);\n") != std::string::npos);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial Refactoring - Extract Method: Extracted Loop With Internal Break")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int sum = 0;\n"
        "    for (int i = 0; i < 100; ++i)\n"
        "    {\n"
        "        sum += i;\n"
        "        if (sum > 50) break;\n"
        "    }\n"
        "    Print(sum);\n"
        "}\n";

    RefactorTestEnvironment env(code);
    // Select entire for-loop (lines 3 to 7)
    lsp::Range range{ { 3, 4 }, { 7, 5 } };
    auto actions = env.CodeActions(range);

    REQUIRE(actions.has_value());
    bool foundExtract = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Method")
        {
            foundExtract = true;
            REQUIRE(action.edit.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);
            MESSAGE("Call edit text: " << edits[0].newText);
            MESSAGE("Method def edit text: " << edits[1].newText);
            CHECK(edits[0].newText.find("sum = NewMethod(sum);") != std::string::npos);
            CHECK(edits[1].newText.find("int NewMethod(int sum)") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial Refactoring - Add const: Interface Method Without Body")
{
    std::string code =
        "interface IWeapon\n"
        "{\n"
        "    void Fire();\n"
        "}\n";

    RefactorTestEnvironment env(code);
    lsp::Range range{ { 2, 9 }, { 2, 9 } };
    auto actions = env.CodeActions(range);

    // Interface methods shouldn't offer add const intention since they have no class body or implementation
    if (actions.has_value())
    {
        for (const auto &act : *actions)
        {
            CHECK(act.title != "Add 'const' qualifier to method");
        }
    }
}


