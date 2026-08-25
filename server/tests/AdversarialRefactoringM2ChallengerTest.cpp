#include <doctest/doctest.h>

#include "features/code_action/CodeActionHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

#include <string>
#include <vector>
#include <optional>
#include <algorithm>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    /**
     * @brief Test harness environment for running adversarial code action and refactoring tests.
     */
    struct AdversarialM2Env
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///adversarial_test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        AdversarialM2Env(const std::string &code)
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

        ~AdversarialM2Env()
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

        bool HasActionWithTitle(const std::string &title, lsp::Range range, lsp::CodeActionContext context = {})
        {
            auto actions = CodeActions(range, context);
            if (!actions.has_value())
            {
                return false;
            }
            return std::any_of(actions->begin(), actions->end(), [&](const lsp::CodeAction &act) {
                return act.title == title;
            });
        }
    };
}

// =============================================================================
// 1. Boundary conditions, malformed ranges, zero-length ranges, keywords & whitespace
// =============================================================================

TEST_CASE("Adversarial M2 - Malformed and Out-of-Bounds Ranges")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int a = 10;\n"
        "    int b = a + 20;\n"
        "}\n";

    AdversarialM2Env env(code);

    // 1. Completely out-of-bounds line numbers (e.g. line 9999)
    CHECK_NOTHROW({
        auto actions = env.CodeActions(lsp::Range{ { 9999, 0 }, { 9999, 10 } });
        // Should not crash and shouldn't extract variable or method
        if (actions.has_value())
        {
            for (const auto &act : *actions)
            {
                CHECK(act.title != "Extract Variable");
                CHECK(act.title != "Extract Method");
            }
        }
    });

    // 2. Inverted line range (startLine > endLine)
    CHECK_NOTHROW({
        auto actions = env.CodeActions(lsp::Range{ { 4, 10 }, { 2, 5 } });
        if (actions.has_value())
        {
            for (const auto &act : *actions)
            {
                CHECK(act.title != "Extract Variable");
            }
        }
    });

    // 3. Inverted character range on same line (startCol > endCol)
    CHECK_NOTHROW({
        auto actions = env.CodeActions(lsp::Range{ { 3, 20 }, { 3, 5 } });
        if (actions.has_value())
        {
            for (const auto &act : *actions)
            {
                CHECK(act.title != "Extract Variable");
            }
        }
    });

    // 4. Zero-length range at (0, 0)
    CHECK_NOTHROW({
        auto actions = env.CodeActions(lsp::Range{ { 0, 0 }, { 0, 0 } });
        if (actions.has_value())
        {
            for (const auto &act : *actions)
            {
                CHECK(act.title != "Extract Variable");
            }
        }
    });

    // 5. Selection over pure whitespace between tokens
    // Line 1 is "{\n", column 0 is '{', line 2 has 4 spaces indent before "int"
    CHECK_NOTHROW({
        auto actions = env.CodeActions(lsp::Range{ { 2, 0 }, { 2, 3 } });
        if (actions.has_value())
        {
            for (const auto &act : *actions)
            {
                CHECK(act.title != "Extract Variable");
            }
        }
    });

    // 6. Selection over keywords
    // "void" on line 0, cols 0..4
    CHECK_FALSE(env.HasActionWithTitle("Extract Variable", lsp::Range{ { 0, 0 }, { 0, 4 } }));
}

TEST_CASE("Adversarial M2 - Null Tree or Empty Code Request Safety")
{
    SymbolTable table;
    ScopeIndex scopeIndex;
    lsp::Range range{ { 0, 0 }, { 0, 0 } };
    lsp::CodeActionContext context;

    // Null tree
    std::string code = "void main() {}";
    CodeActionRequest nullTreeReq{ "file:///test.as", code, nullptr, range, context, table, scopeIndex };
    auto nullTreeRes = GetCodeActions(nullTreeReq);
    CHECK_FALSE(nullTreeRes.has_value());

    // Empty source code
    AngelScriptParser parser;
    std::string emptyCode = "";
    TSTree *emptyTree = parser.Parse(emptyCode);
    CodeActionRequest emptyCodeReq{ "file:///test.as", emptyCode, emptyTree, range, context, table, scopeIndex };
    auto emptyCodeRes = GetCodeActions(emptyCodeReq);
    CHECK_FALSE(emptyCodeRes.has_value());
    if (emptyTree)
    {
        ts_tree_delete(emptyTree);
    }
}

// =============================================================================
// 2. Extract Variable - LHS of assignments, declarations, class headers
// =============================================================================

TEST_CASE("Adversarial M2 - Extract Variable Rejections")
{
    std::string code =
        "class Player\n"
        "{\n"
        "    int health;\n"
        "    void TakeDamage(int dmg)\n"
        "    {\n"
        "        health = health - dmg;\n"
        "        int temp = 5;\n"
        "    }\n"
        "}\n";

    AdversarialM2Env env(code);

    // 1. LHS of assignment: "health" on line 5, cols 8..14
    CHECK_FALSE(env.HasActionWithTitle("Extract Variable", lsp::Range{ { 5, 8 }, { 5, 14 } }));

    // 2. Declaration statement: "int temp = 5;" on line 6, cols 8..21
    CHECK_FALSE(env.HasActionWithTitle("Extract Variable", lsp::Range{ { 6, 8 }, { 6, 21 } }));

    // 3. Type specifier in declaration: "int" on line 6, cols 8..11
    CHECK_FALSE(env.HasActionWithTitle("Extract Variable", lsp::Range{ { 6, 8 }, { 6, 11 } }));

    // 4. Class header: "class Player" on line 0, cols 0..12
    CHECK_FALSE(env.HasActionWithTitle("Extract Variable", lsp::Range{ { 0, 0 }, { 0, 12 } }));

    // 5. Function header: "void TakeDamage(int dmg)" on line 3, cols 4..28
    CHECK_FALSE(env.HasActionWithTitle("Extract Variable", lsp::Range{ { 3, 4 }, { 3, 28 } }));

    // 6. Valid RHS expression: "health - dmg" on line 5, cols 17..29 SHOULD be extractable
    CHECK(env.HasActionWithTitle("Extract Variable", lsp::Range{ { 5, 17 }, { 5, 29 } }));
}

// =============================================================================
// 3. Extract Method - Multiple functions, unclosed blocks, non-statement ranges
// =============================================================================

TEST_CASE("Adversarial M2 - Extract Method Multi-Function and Out-of-Scope Rejection")
{
    std::string code =
        "void FunctionOne()\n"
        "{\n"
        "    int a = 1;\n"
        "    int b = 2;\n"
        "}\n"
        "\n"
        "void FunctionTwo()\n"
        "{\n"
        "    int c = 3;\n"
        "    int d = 4;\n"
        "}\n";

    AdversarialM2Env env(code);

    // 1. Spanning across FunctionOne and FunctionTwo (line 3 to line 8)
    CHECK_FALSE(env.HasActionWithTitle("Extract Method", lsp::Range{ { 3, 4 }, { 8, 14 } }));

    // 2. Selecting outside of any function (line 5 to line 6)
    CHECK_FALSE(env.HasActionWithTitle("Extract Method", lsp::Range{ { 5, 0 }, { 6, 0 } }));

    // 3. Valid selection inside FunctionOne (lines 2 to 3) SHOULD offer Extract Method
    CHECK(env.HasActionWithTitle("Extract Method", lsp::Range{ { 2, 4 }, { 3, 14 } }));
}

TEST_CASE("Adversarial M2 - Extract Method with Unclosed Blocks and Syntax Errors")
{
    // Malformed code with unclosed braces and syntax errors
    std::string brokenCode =
        "void BrokenFunction()\n"
        "{\n"
        "    if (true) {\n"
        "        int x = 10;\n"
        "        int y = x + 5;\n"; // missing closing braces

    AdversarialM2Env env(brokenCode);

    // Request Extract Method on statements within the broken function - must not crash
    CHECK_NOTHROW({
        auto actions = env.CodeActions(lsp::Range{ { 3, 8 }, { 4, 22 } });
        // May or may not produce action depending on tree recovery, but MUST NOT crash
    });
}

// =============================================================================
// 4. Sort and Clean #include Directives
// =============================================================================

TEST_CASE("Adversarial M2 - Sort and Clean Includes: No Includes in Document")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    int x = 42;\n"
        "}\n";

    AdversarialM2Env env(code);

    // When there are no includes, Sort and Clean action should NOT be presented
    CHECK_FALSE(env.HasActionWithTitle("Sort and Clean #include Directives", lsp::Range{ { 0, 0 }, { 0, 0 } }));
}

TEST_CASE("Adversarial M2 - Sort and Clean Includes: All Unresolved and Inline Comments")
{
    // Three unresolved includes in random order with duplicates and inline comments
    std::string code =
        "#include \"zeta.as\" // trailing comment for zeta\n"
        "#include <beta.as> /* block comment */\n"
        "#include \"alpha.as\"\n"
        "#include \"zeta.as\" // duplicate zeta\n"
        "#include <alpha_sys.as>\n"
        "\n"
        "void main()\n"
        "{\n"
        "}\n";

    AdversarialM2Env env(code);

    auto actions = env.CodeActions(lsp::Range{ { 0, 0 }, { 4, 23 } });
    REQUIRE(actions.has_value());

    bool foundSort = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Sort and Clean #include Directives")
        {
            foundSort = true;
            CHECK(action.kind.has_value());
            CHECK(action.kind.value() == lsp::CodeActionKind::SourceOrganizeImports);
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());

            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 1);

            const std::string &newText = edits[0].newText;

            // 1. Angled headers should come first, sorted alphabetically (<alpha_sys.as>, then <beta.as>)
            size_t posAlphaSys = newText.find("#include <alpha_sys.as>");
            size_t posBeta = newText.find("#include <beta.as>");
            CHECK(posAlphaSys != std::string::npos);
            CHECK(posBeta != std::string::npos);
            CHECK(posAlphaSys < posBeta);

            // 2. Blank line separating angled and quoted
            CHECK(newText.find(">\n\n#include \"") != std::string::npos);

            // 3. Quoted headers should be sorted alphabetically ("alpha.as", then "zeta.as")
            size_t posAlpha = newText.find("#include \"alpha.as\"");
            size_t posZeta = newText.find("#include \"zeta.as\"");
            CHECK(posAlpha != std::string::npos);
            CHECK(posZeta != std::string::npos);
            CHECK(posBeta < posAlpha);
            CHECK(posAlpha < posZeta);

            // 4. Duplicate "zeta.as" must be deduplicated (only appears once)
            size_t firstZeta = newText.find("#include \"zeta.as\"");
            size_t secondZeta = newText.find("#include \"zeta.as\"", firstZeta + 1);
            CHECK(secondZeta == std::string::npos);

            // 5. Replaced range must span from line 0 to line 5
            CHECK(edits[0].range.start.line == 0);
            CHECK(edits[0].range.end.line == 5);
        }
    }
    CHECK(foundSort);
}

// =============================================================================
// 5. Getter/Setter & Const Qualifier Edge Cases
// =============================================================================

TEST_CASE("Adversarial M2 - Getter/Setter on Class without Fields or Global Variable")
{
    std::string code =
        "int globalVar = 100;\n"
        "\n"
        "class EmptyClass\n"
        "{\n"
        "}\n";

    AdversarialM2Env env(code);

    // Cursor on global variable (line 0)
    CHECK_FALSE(env.HasActionWithTitle("Generate Getter", lsp::Range{ { 0, 4 }, { 0, 13 } }));
    CHECK_FALSE(env.HasActionWithTitle("Generate Setter", lsp::Range{ { 0, 4 }, { 0, 13 } }));

    // Cursor on EmptyClass (line 2)
    CHECK_FALSE(env.HasActionWithTitle("Generate Getter", lsp::Range{ { 2, 6 }, { 2, 16 } }));
    CHECK_FALSE(env.HasActionWithTitle("Generate Setter", lsp::Range{ { 2, 6 }, { 2, 16 } }));
}

TEST_CASE("Adversarial M2 - Const Qualifier Intention on Mutating vs Const Method")
{
    std::string code =
        "class Inventory\n"
        "{\n"
        "    int itemCount;\n"
        "    void AddItem() { itemCount += 1; }\n"
        "    int GetCount() { return itemCount; }\n"
        "    int GetCountConst() const { return itemCount; }\n"
        "}\n";

    AdversarialM2Env env(code);

    // 1. Mutating method "AddItem" (line 3) -> MUST NOT offer "Add 'const' qualifier to method"
    CHECK_FALSE(env.HasActionWithTitle("Add 'const' qualifier to method", lsp::Range{ { 3, 9 }, { 3, 16 } }));

    // 2. Already const method "GetCountConst" (line 5) -> MUST NOT offer "Add 'const' qualifier to method"
    CHECK_FALSE(env.HasActionWithTitle("Add 'const' qualifier to method", lsp::Range{ { 5, 8 }, { 5, 21 } }));

    // 3. Non-mutating read-only method "GetCount" (line 4) -> SHOULD offer "Add 'const' qualifier to method"
    CHECK(env.HasActionWithTitle("Add 'const' qualifier to method", lsp::Range{ { 4, 8 }, { 4, 16 } }));
}

TEST_CASE("Adversarial M2 - Deeply Nested Extract Variable Indentation and Placement")
{
    std::string code =
        "void Outer()\n"
        "{\n"
        "    if (true)\n"
        "    {\n"
        "        while (true)\n"
        "        {\n"
        "            int result = (10 + 20) * 3;\n"
        "        }\n"
        "    }\n"
        "}\n";

    AdversarialM2Env env(code);

    // Select "(10 + 20)" on line 6, cols 25 to 34
    lsp::Range range{ { 6, 25 }, { 6, 34 } };
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

            // The declaration must be inserted at line 6 with 12 spaces indentation (matching while block body)
            CHECK(edits[0].range.start.line == 6);
            CHECK(edits[0].range.start.character == 0);
            CHECK(edits[0].newText.starts_with("            "));
            CHECK(edits[0].newText.find("newVar = (10 + 20);\n") != std::string::npos);

            // Replacement at line 6
            CHECK(edits[1].range.start.line == 6);
            CHECK(edits[1].newText == "newVar");
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial M2 - Extract Method with Multi-Variable Outputs (&out parameters)")
{
    std::string code =
        "void Calculate()\n"
        "{\n"
        "    int a = 10;\n"
        "    int x = a + 1;\n"
        "    int y = a + 2;\n"
        "    int z = x + y;\n"
        "}\n";

    AdversarialM2Env env(code);

    // Select lines 3 to 4: "int x = a + 1;\n    int y = a + 2;"
    lsp::Range range{ { 3, 4 }, { 4, 18 } };
    auto actions = env.CodeActions(range);
    REQUIRE(actions.has_value());

    bool foundMethod = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Extract Method")
        {
            foundMethod = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Edit 0: Call site invocation (callEdit)
            const std::string &callSite = edits[0].newText;
            CHECK(callSite.find("NewMethod") != std::string::npos);

            // Edit 1: Definition of NewMethod containing &out parameter (methodDefEdit)
            const std::string &fnDef = edits[1].newText;
            CHECK(fnDef.find("&out") != std::string::npos);
            CHECK(fnDef.find("return") != std::string::npos);
        }
    }
    CHECK(foundMethod);
}

TEST_CASE("Adversarial M2 - Missing Const Qualifier Quick-Fix via Diagnostic")
{
    std::string code =
        "class Renderer\n"
        "{\n"
        "    void Draw()\n"
        "    {\n"
        "    }\n"
        "}\n"
        "void RenderScene(const Renderer@ r)\n"
        "{\n"
        "    r.Draw();\n"
        "}\n";

    AdversarialM2Env env(code);

    // Construct diagnostic for "as-err-const-method-required" on line 8 (r.Draw())
    lsp::Diagnostic diag;
    diag.code = "as-err-const-method-required";
    diag.message = "Method Draw() is not marked const";
    diag.range = lsp::Range{ { 8, 6 }, { 8, 10 } };

    lsp::CodeActionContext ctx;
    ctx.diagnostics = { diag };

    auto actions = env.CodeActions(diag.range, ctx);
    REQUIRE(actions.has_value());

    bool foundQuickFix = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Add 'const' qualifier to method")
        {
            foundQuickFix = true;
            CHECK(action.kind.value() == lsp::CodeActionKind::QuickFix);
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 1);

            // Should insert " const" right after parameter_list on line 2 (void Draw())
            CHECK(edits[0].range.start.line == 2);
            CHECK(edits[0].newText == " const");
        }
    }
    CHECK(foundQuickFix);
}

TEST_CASE("Adversarial M2 Bug 1 - Extract Method Captures Enclosing Class Member Fields As Parameters")
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

    AdversarialM2Env env(code);
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
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Bug: CodeActionHandler extracts member variables into parameter list: "void NewMethod(int amount, auto m_armor, auto m_health)"
            // Expected: Only parameter `amount` should be in the extracted member method signature
            CHECK(edits[0].newText.find("NewMethod(amount);") != std::string::npos);
            CHECK(edits[1].newText.find("void NewMethod(int amount)") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}

TEST_CASE("Adversarial M2 Bug 2 - Extract Method Misses Output Variable When Declared Before Range And Mutated Inside")
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

    AdversarialM2Env env(code);
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
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(edits.size() == 2);

            // Bug: `sum` is passed by value: "void NewMethod(int sum)" and call site is "NewMethod(sum);", discarding mutations!
            // Expected: `sum` must be returned "sum = NewMethod(sum);" with "int NewMethod(int sum)" returning sum
            CHECK(edits[0].newText.find("sum = NewMethod(sum);") != std::string::npos);
            CHECK(edits[1].newText.find("int NewMethod(int sum)") != std::string::npos);
        }
    }
    CHECK(foundExtract);
}


