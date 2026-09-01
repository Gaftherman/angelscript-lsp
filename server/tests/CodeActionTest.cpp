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

        std::optional<std::vector<lsp::CodeAction>> CodeActions(
            lsp::Range range = lsp::Range{ {0, 0}, {0, 0} },
            lsp::CodeActionContext context = lsp::CodeActionContext{})
        {
            CodeActionRequest req{ uri, sourceCode, tree, range, context, symbolTable, scopeIndex };
            return GetCodeActions(req);
        }
    };
}

TEST_CASE("CodeActionHandler - Remove Unused Local Variable (Single Declaration)")
{
    std::string code =
        "void main() {\n"
        "    int unusedVar = 42;\n"
        "    float active = 1.0f;\n"
        "    active += 2.0f;\n"
        "}\n";

    TestEnvironment env(code);
    lsp::Range r{ {1, 4}, {1, 18} };
    auto actions = env.CodeActions(r);

    REQUIRE(actions.has_value());
    REQUIRE(!actions->empty());

    bool foundAction = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Remove unused variable 'unusedVar'")
        {
            foundAction = true;
            CHECK(action.kind.has_value());
            bool isQuickFix = (action.kind.value() == lsp::CodeActionKind::QuickFix);
            CHECK(isQuickFix);
            CHECK(action.isPreferred.has_value());
            CHECK(action.isPreferred.value() == true);
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            REQUIRE(changes.contains(lsp::DocumentUri::parse(env.uri)));
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].range.start.line == 1);
            CHECK(edits[0].range.start.character == 0);
            CHECK(edits[0].range.end.line == 2);
            CHECK(edits[0].range.end.character == 0);
            CHECK(edits[0].newText.empty());
        }
    }

    CHECK(foundAction);
}

TEST_CASE("CodeActionHandler - Remove Unused Local Variable (Multi Declarator)")
{
    std::string code =
        "void main() {\n"
        "    int a = 1, unused = 2, b = 3;\n"
        "    a += b;\n"
        "}\n";

    TestEnvironment env(code);
    lsp::Range r{ {1, 15}, {1, 25} };
    auto actions = env.CodeActions(r);

    REQUIRE(actions.has_value());
    bool foundAction = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Remove unused variable 'unused'")
        {
            foundAction = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.empty());
        }
    }

    CHECK(foundAction);
}

TEST_CASE("CodeActionHandler - Implement Missing Interface Methods")
{
    std::string code =
        "interface IWeapon {\n"
        "    void Fire();\n"
        "    int GetAmmo();\n"
        "}\n"
        "class Gun : IWeapon {\n"
        "}\n";

    TestEnvironment env(code);
    lsp::Range r{ {4, 0}, {5, 1} };
    auto actions = env.CodeActions(r);

    REQUIRE(actions.has_value());
    bool foundAction = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Implement missing interface methods for 'IWeapon'")
        {
            foundAction = true;
            CHECK(action.kind.has_value());
            bool isQuickFix = (action.kind.value() == lsp::CodeActionKind::QuickFix);
            CHECK(isQuickFix);
            CHECK(action.isPreferred.has_value());
            CHECK(action.isPreferred.value() == true);
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.find("void Fire()") != std::string::npos);
            CHECK(edits[0].newText.find("int GetAmmo()") != std::string::npos);
            CHECK(edits[0].newText.find("return 0;") != std::string::npos);
        }
    }

    CHECK(foundAction);
}

TEST_CASE("CodeActionHandler - Partially Implemented Interface")
{
    std::string code =
        "interface IWeapon {\n"
        "    void Fire();\n"
        "    int GetAmmo();\n"
        "}\n"
        "class Gun : IWeapon {\n"
        "    void Fire() {}\n"
        "}\n";

    TestEnvironment env(code);
    lsp::Range r{ {4, 0}, {6, 1} };
    auto actions = env.CodeActions(r);

    REQUIRE(actions.has_value());
    bool foundAction = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Implement missing interface methods for 'IWeapon'")
        {
            foundAction = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.find("int GetAmmo()") != std::string::npos);
            CHECK(edits[0].newText.find("void Fire()") == std::string::npos);
        }
    }

    CHECK(foundAction);
}

TEST_CASE("CodeActionHandler - Fully Implemented Interface Returns No Interface Action")
{
    std::string code =
        "interface IWeapon {\n"
        "    void Fire();\n"
        "    int GetAmmo();\n"
        "}\n"
        "class Gun : IWeapon {\n"
        "    void Fire() {}\n"
        "    int GetAmmo() { return 10; }\n"
        "}\n";

    TestEnvironment env(code);
    lsp::Range r{ {4, 0}, {7, 1} };
    auto actions = env.CodeActions(r);

    if (actions.has_value())
    {
        for (const auto &action : *actions)
        {
            CHECK(action.title != "Implement missing interface methods for 'IWeapon'");
        }
    }
}

TEST_CASE("CodeActionHandler - Inherited Interfaces Method Generation")
{
    std::string code =
        "interface IBase {\n"
        "    void BaseMethod();\n"
        "}\n"
        "interface IDerived : IBase {\n"
        "    void DerivedMethod();\n"
        "}\n"
        "class MyClass : IDerived {\n"
        "}\n";

    TestEnvironment env(code);
    lsp::Range r{ {6, 0}, {7, 1} };
    auto actions = env.CodeActions(r);

    REQUIRE(actions.has_value());
    bool foundAction = false;
    for (const auto &action : *actions)
    {
        if (action.title == "Implement missing interface methods for 'IDerived'")
        {
            foundAction = true;
            REQUIRE(action.edit.has_value());
            REQUIRE(action.edit->changes.has_value());
            auto changes = action.edit->changes.value();
            const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
            REQUIRE(!edits.empty());
            CHECK(edits[0].newText.find("void BaseMethod()") != std::string::npos);
            CHECK(edits[0].newText.find("void DerivedMethod()") != std::string::npos);
        }
    }

    CHECK(foundAction);
}

TEST_CASE("CodeActionHandler - Robustness on Null Tree / Empty Range")
{
    CodeActionRequest req{ "file:///empty.as", "", nullptr, lsp::Range{}, lsp::CodeActionContext{}, SymbolTable{}, ScopeIndex{} };
    auto actions = GetCodeActions(req);
    CHECK(!actions.has_value());
}

TEST_CASE("CodeActionHandler - ResolveCodeAction returns resolved code action")
{
    lsp::CodeAction action;
    action.title = "Test Action";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

    CodeActionResolveRequest req{ action, SymbolTable{}, ScopeIndex{} };
    auto resolved = ResolveCodeAction(req);
    REQUIRE(resolved.has_value());
    CHECK(resolved->title == "Test Action");
}


// =====================================================================================
// "Did you mean 'X'?" for an undefined identifier.
//
// Of the ~130 diagnostic codes this server emits, exactly one had a quick fix bound to it before
// this. Undefined identifier is the one worth having next - it is what a typo looks like, and the
// name the user meant is nearly always already in the symbol table.
//
// What these tests defend is mostly the silence. A wrong suggestion is worse than no suggestion:
// the user reads "did you mean" as the server knowing something. So the threshold is tight, and
// most of what follows checks that nothing is offered when nothing is close.
// =====================================================================================

namespace
{
    /** @brief A CodeActionContext carrying one undefined-identifier diagnostic over a range. */
    lsp::CodeActionContext UndefinedIdentifierAt(lsp::Range range)
    {
        lsp::Diagnostic diag;
        diag.range = range;
        diag.code = lsp::String("as-err-undefined-identifier");
        diag.message = "Undefined identifier";

        lsp::CodeActionContext context;
        context.diagnostics.push_back(diag);
        return context;
    }

    /** @brief Titles of every "Did you mean" action, in the order they were offered. */
    std::vector<std::string> SuggestionTitles(const std::optional<std::vector<lsp::CodeAction>> &actions)
    {
        std::vector<std::string> titles;
        if (!actions.has_value())
        {
            return titles;
        }
        for (const auto &action : *actions)
        {
            if (action.title.rfind("Did you mean", 0) == 0)
            {
                titles.push_back(action.title);
            }
        }
        return titles;
    }
}

TEST_CASE("CodeActionHandler - Suggests the local whose name was mistyped")
{
    std::string code =
        "void main() {\n"
        "    int counter = 0;\n"
        "    countor = 1;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 11} };
    auto actions = env.CodeActions(typo, UndefinedIdentifierAt(typo));

    const auto titles = SuggestionTitles(actions);
    REQUIRE_FALSE(titles.empty());
    CHECK(titles[0] == "Did you mean 'counter'?");
}

TEST_CASE("CodeActionHandler - The suggestion replaces exactly the identifier")
{
    // The edit is built from the AST node, not from the diagnostic's range, so it stays right even
    // when the diagnostic covers more or less than the name.
    std::string code =
        "void main() {\n"
        "    int counter = 0;\n"
        "    countor = 1;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 11} };
    auto actions = env.CodeActions(typo, UndefinedIdentifierAt(typo));

    REQUIRE(actions.has_value());
    const lsp::CodeAction *suggestion = nullptr;
    for (const auto &action : *actions)
    {
        if (action.title == "Did you mean 'counter'?")
        {
            suggestion = &action;
            break;
        }
    }
    REQUIRE(suggestion != nullptr);
    REQUIRE(suggestion->edit.has_value());
    REQUIRE(suggestion->edit->changes.has_value());

    const auto &edits = suggestion->edit->changes->begin()->second;
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].newText == "counter");
    CHECK(edits[0].range.start.line == 2);
    CHECK(edits[0].range.start.character == 4);
    CHECK(edits[0].range.end.character == 11);
}

TEST_CASE("CodeActionHandler - A case-only difference is suggested")
{
    // The most common miss of all, and the reason the comparison is case-folded rather than exact.
    std::string code =
        "void main() {\n"
        "    int myVariable = 0;\n"
        "    myvariable = 1;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 14} };
    auto actions = env.CodeActions(typo, UndefinedIdentifierAt(typo));

    const auto titles = SuggestionTitles(actions);
    REQUIRE_FALSE(titles.empty());
    CHECK(titles[0] == "Did you mean 'myVariable'?");
}

TEST_CASE("CodeActionHandler - A global function is suggested as readily as a local")
{
    std::string code =
        "void CalculateDamage() { }\n"
        "void main() {\n"
        "    CalculateDamag();\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 19} };
    auto actions = env.CodeActions(typo, UndefinedIdentifierAt(typo));

    const auto titles = SuggestionTitles(actions);
    REQUIRE_FALSE(titles.empty());
    CHECK(titles[0] == "Did you mean 'CalculateDamage'?");
}

TEST_CASE("CodeActionHandler - Nothing is suggested when nothing is close")
{
    // The case that matters most. A name that resembles nothing gets silence, not the nearest
    // string in the workspace.
    std::string code =
        "void main() {\n"
        "    int counter = 0;\n"
        "    zzzzqqqqwwww = 1;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 16} };
    auto actions = env.CodeActions(typo, UndefinedIdentifierAt(typo));

    CHECK(SuggestionTitles(actions).empty());
}

TEST_CASE("CodeActionHandler - A short name is not matched loosely")
{
    // `ab` and `xy` are two edits apart, which is the whole of a two-letter name. At that length a
    // difference is not a typo, it is a different name.
    std::string code =
        "void main() {\n"
        "    int ab = 0;\n"
        "    xy = 1;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 6} };
    auto actions = env.CodeActions(typo, UndefinedIdentifierAt(typo));

    CHECK(SuggestionTitles(actions).empty());
}

TEST_CASE("CodeActionHandler - No suggestion without an undefined-identifier diagnostic")
{
    // The quick fix is bound to the diagnostic, not to the cursor. Without one, the same position
    // offers nothing - otherwise every identifier in the file would sprout a menu of near-misses.
    std::string code =
        "void main() {\n"
        "    int counter = 0;\n"
        "    countor = 1;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 11} };
    auto actions = env.CodeActions(typo, lsp::CodeActionContext{});

    CHECK(SuggestionTitles(actions).empty());
}

TEST_CASE("CodeActionHandler - A tie offers no preferred fix")
{
    // Two candidates one edit away. Marking a preferred action is what lets an editor apply it
    // without asking, so a tie must not have one.
    std::string code =
        "void main() {\n"
        "    int cat = 0;\n"
        "    int cut = 0;\n"
        "    cot = 1;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {3, 4}, {3, 7} };
    auto actions = env.CodeActions(typo, UndefinedIdentifierAt(typo));

    REQUIRE(actions.has_value());
    for (const auto &action : *actions)
    {
        if (action.title.rfind("Did you mean", 0) == 0)
        {
            CHECK_FALSE(action.isPreferred.value_or(false));
        }
    }
}

TEST_CASE("CodeActionHandler - At most three suggestions are offered")
{
    // Five names within the threshold, so the cap is what limits the list rather than the
    // threshold doing it incidentally.
    std::string code =
        "void main() {\n"
        "    int value = 0;\n"
        "    int valus = 0;\n"
        "    int valui = 0;\n"
        "    int valuz = 0;\n"
        "    int valve = 0;\n"
        "    valuu = 1;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {6, 4}, {6, 9} };
    auto actions = env.CodeActions(typo, UndefinedIdentifierAt(typo));

    CHECK(SuggestionTitles(actions).size() == 3);
}

// =====================================================================================
// Quick fixes bound to the diagnostics that ask for them.
//
// A diagnostic-driven fix is not the same thing as an action that happens to be available where
// the diagnostic sits. An editor grouping fixes under a problem, or asking for actions at a
// diagnostic's range, only finds an action that names the diagnostic in its `diagnostics` field.
// =====================================================================================

namespace
{
    /** @brief A CodeActionContext carrying one diagnostic of the given code over a range. */
    lsp::CodeActionContext DiagnosticAt(lsp::Range range, const std::string &code)
    {
        lsp::Diagnostic diag;
        diag.range = range;
        diag.code = lsp::String(code);
        diag.message = code;

        lsp::CodeActionContext context;
        context.diagnostics.push_back(diag);
        return context;
    }

    /** @brief The first action whose title starts with the given prefix, or nullptr. */
    const lsp::CodeAction *ActionTitled(const std::optional<std::vector<lsp::CodeAction>> &actions,
                                        const std::string &prefix)
    {
        if (!actions.has_value())
        {
            return nullptr;
        }
        for (const auto &action : *actions)
        {
            if (action.title.rfind(prefix, 0) == 0)
            {
                return &action;
            }
        }
        return nullptr;
    }
}

TEST_CASE("CodeActionHandler - Suggests the type whose name was mistyped")
{
    std::string code =
        "class PlayerController { }\n"
        "void main() {\n"
        "    PlayerControler pc;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 19} };
    auto actions = env.CodeActions(typo, DiagnosticAt(typo, "as-err-unresolved-type"));

    const auto *suggestion = ActionTitled(actions, "Did you mean");
    REQUIRE(suggestion != nullptr);
    CHECK(suggestion->title == "Did you mean 'PlayerController'?");
}

TEST_CASE("CodeActionHandler - A function name is not offered where a type was written")
{
    // The candidate set is what separates the two cases. `Calculat` is one edit from the function
    // `Calculate`, but a function cannot stand where a type name goes, so suggesting it would be
    // offering something that cannot compile.
    std::string code =
        "void Calculate() { }\n"
        "void main() {\n"
        "    Calculat value;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 12} };
    auto actions = env.CodeActions(typo, DiagnosticAt(typo, "as-err-unresolved-type"));

    CHECK(ActionTitled(actions, "Did you mean") == nullptr);
}

TEST_CASE("CodeActionHandler - A function name IS offered for a plain undefined identifier")
{
    // The same workspace, the other diagnostic. Here a function is exactly what the user meant.
    std::string code =
        "void Calculate() { }\n"
        "void main() {\n"
        "    Calculat();\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range typo{ {2, 4}, {2, 12} };
    auto actions = env.CodeActions(typo, DiagnosticAt(typo, "as-err-undefined-identifier"));

    const auto *suggestion = ActionTitled(actions, "Did you mean");
    REQUIRE(suggestion != nullptr);
    CHECK(suggestion->title == "Did you mean 'Calculate'?");
}

TEST_CASE("CodeActionHandler - Removes the '@' from a handle on a primitive")
{
    // There is no handle to a primitive in AngelScript, so there is exactly one thing the user can
    // have meant and the fix is to delete one character.
    std::string code =
        "void main() {\n"
        "    int@ h = null;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range at{ {1, 4}, {1, 8} };
    auto actions = env.CodeActions(at, DiagnosticAt(at, "as-err-handle-on-primitive"));

    const auto *fix = ActionTitled(actions, "Remove '@'");
    REQUIRE(fix != nullptr);
    REQUIRE(fix->edit.has_value());
    REQUIRE(fix->edit->changes.has_value());

    const auto &edits = fix->edit->changes->begin()->second;
    REQUIRE(edits.size() == 1);

    // Exactly the '@', nothing around it.
    CHECK(edits[0].newText.empty());
    CHECK(edits[0].range.start.line == 1);
    CHECK(edits[0].range.start.character == 7);
    CHECK(edits[0].range.end.character == 8);
}

TEST_CASE("CodeActionHandler - Finds the '@' when it is spaced away from the type")
{
    std::string code =
        "void main() {\n"
        "    int @h = null;\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range at{ {1, 4}, {1, 9} };
    auto actions = env.CodeActions(at, DiagnosticAt(at, "as-err-handle-on-primitive"));

    const auto *fix = ActionTitled(actions, "Remove '@'");
    REQUIRE(fix != nullptr);

    const auto &edits = fix->edit->changes->begin()->second;
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].range.start.character == 8);
    CHECK(edits[0].range.end.character == 9);
}

TEST_CASE("CodeActionHandler - The interface fix names the diagnostic it fixes")
{
    // It was already offered when the cursor sat in the class. What it never did was say which
    // problem it solves, so an editor asking for the fixes for that problem found none.
    std::string code =
        "interface IThinker {\n"
        "    void Think();\n"
        "}\n"
        "class Robot : IThinker {\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range atClass{ {3, 0}, {3, 22} };
    auto actions = env.CodeActions(atClass, DiagnosticAt(atClass, "as-err-interface-impl-missing"));

    const auto *fix = ActionTitled(actions, "Implement missing interface methods");
    REQUIRE(fix != nullptr);
    REQUIRE(fix->diagnostics.has_value());
    REQUIRE(fix->diagnostics->size() == 1);

    const auto &named = (*fix->diagnostics)[0];
    REQUIRE(named.code.has_value());
    CHECK(std::get<lsp::String>(named.code.value()) == "as-err-interface-impl-missing");
}

// =====================================================================================
// "Did you mean this file?" for an #include that resolves to nothing.
//
// The candidates are the files the server has actually indexed. That is the honest set - a file it
// has never heard of is one it cannot vouch for - and it makes the common case, a file that moved
// to another directory under the same name, land at distance zero.
// =====================================================================================

namespace
{
    /**
     * @brief A workspace of several files, so an include can be pointed at one of the others.
     *
     * TestEnvironment above indexes exactly one document, which is enough for every rule that reads
     * a single file. An include suggestion is about the relationship between two.
     */
    struct MultiFileEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri;
        std::string sourceCode;
        TSTree *tree = nullptr;

        MultiFileEnvironment(const std::string &mainUri, const std::string &mainCode)
            : uri(mainUri), sourceCode(mainCode)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        ~MultiFileEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        /** @brief Indexes another file, the way the server's include-closure walk would. */
        void AddFile(const std::string &otherUri, const std::string &code)
        {
            symbolCollector.CollectSymbols(otherUri, code, parser, symbolTable);
        }

        std::optional<std::vector<lsp::CodeAction>> CodeActions(lsp::Range range, lsp::CodeActionContext context)
        {
            CodeActionRequest req{ uri, sourceCode, tree, range, context, symbolTable, scopeIndex };
            return GetCodeActions(req);
        }
    };
}

TEST_CASE("CodeActionHandler - Points a broken #include at the file that moved")
{
    // The name is unchanged and only the directory moved, which is the case that actually happens
    // and the one that lands at distance zero.
    const std::string mainCode =
        "#include \"helper.as\"\n"
        "void main() { }\n";

    MultiFileEnvironment env("file:///project/main.as", mainCode);
    env.AddFile("file:///project/lib/helper.as", "void Helped() { }\n");

    const lsp::Range includeLine{ {0, 0}, {0, 20} };
    auto actions = env.CodeActions(includeLine, DiagnosticAt(includeLine, "as-warn-include-not-found"));

    const auto *fix = ActionTitled(actions, "Did you mean");
    REQUIRE(fix != nullptr);
    CHECK(fix->title == "Did you mean 'lib/helper.as'?");

    REQUIRE(fix->edit.has_value());
    REQUIRE(fix->edit->changes.has_value());
    const auto &edits = fix->edit->changes->begin()->second;
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].newText == "lib/helper.as");

    // Only what is inside the quotes. Rewriting the whole line would take the directive with it.
    CHECK(edits[0].range.start.line == 0);
    CHECK(edits[0].range.start.character == 10);
    CHECK(edits[0].range.end.character == 19);
}

TEST_CASE("CodeActionHandler - Points a broken #include at a near-miss filename")
{
    const std::string mainCode =
        "#include \"helpers.as\"\n"
        "void main() { }\n";

    MultiFileEnvironment env("file:///project/main.as", mainCode);
    env.AddFile("file:///project/helper.as", "void Helped() { }\n");

    const lsp::Range includeLine{ {0, 0}, {0, 21} };
    auto actions = env.CodeActions(includeLine, DiagnosticAt(includeLine, "as-warn-include-not-found"));

    const auto *fix = ActionTitled(actions, "Did you mean");
    REQUIRE(fix != nullptr);
    CHECK(fix->title == "Did you mean 'helper.as'?");
}

TEST_CASE("CodeActionHandler - A broken #include resembling nothing indexed gets no suggestion")
{
    const std::string mainCode =
        "#include \"zzzzqqqqwwww.as\"\n"
        "void main() { }\n";

    MultiFileEnvironment env("file:///project/main.as", mainCode);
    env.AddFile("file:///project/helper.as", "void Helped() { }\n");

    const lsp::Range includeLine{ {0, 0}, {0, 26} };
    auto actions = env.CodeActions(includeLine, DiagnosticAt(includeLine, "as-warn-include-not-found"));

    CHECK(ActionTitled(actions, "Did you mean") == nullptr);
}

TEST_CASE("CodeActionHandler - No include suggestion without the include diagnostic")
{
    // Bound to the diagnostic, not to the cursor sitting on an #include line.
    const std::string mainCode =
        "#include \"helper.as\"\n"
        "void main() { }\n";

    MultiFileEnvironment env("file:///project/main.as", mainCode);
    env.AddFile("file:///project/lib/helper.as", "void Helped() { }\n");

    const lsp::Range includeLine{ {0, 0}, {0, 20} };
    auto actions = env.CodeActions(includeLine, lsp::CodeActionContext{});

    CHECK(ActionTitled(actions, "Did you mean") == nullptr);
}

// =====================================================================================
// The accessor portability hint's quick fix.
//
// Measured against angelscript_oracle, and the measurement is what makes this a fix rather than a
// suggestion: `class C { int get_X() {...} }` used as `c.X` is ACCEPTED under
// asEP_PROPERTY_ACCESSOR_MODE 2 and REJECTED under 3, while the same accessor carrying the
// `property` keyword is accepted under BOTH. So applying this cannot break a build that works
// today - it can only stop one from breaking on a host running the engine's own default.
// =====================================================================================

namespace
{
    /** @brief A context carrying the portability hint over a range. */
    lsp::CodeActionContext AccessorHintAt(lsp::Range range)
    {
        return DiagnosticAt(range, "as-hint-accessor-portability");
    }
}

TEST_CASE("CodeActionHandler - Adds the 'property' keyword to a bare accessor")
{
    std::string code =
        "class C {\n"
        "    int get_X() { return 1; }\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range at{ {1, 4}, {1, 14} };
    auto actions = env.CodeActions(at, AccessorHintAt(at));

    const auto *fix = ActionTitled(actions, "Add the 'property' keyword");
    REQUIRE(fix != nullptr);
    REQUIRE(fix->edit.has_value());
    REQUIRE(fix->edit->changes.has_value());

    const auto &edits = fix->edit->changes->begin()->second;
    REQUIRE(edits.size() == 1);

    // An insertion, not a replacement: nothing existing is touched.
    CHECK(edits[0].newText == " property");
    CHECK(edits[0].range.start.line == edits[0].range.end.line);
    CHECK(edits[0].range.start.character == edits[0].range.end.character);

    // Immediately after the parameter list's ')'. On `    int get_X() { return 1; }` the ')' sits
    // at column 14, so the insertion point - one past it - is 15.
    CHECK(edits[0].range.start.line == 1);
    CHECK(edits[0].range.start.character == 15);
}

TEST_CASE("CodeActionHandler - The accessor fix produces text the compiler accepts")
{
    // Applying the edit by hand and reading the result back, so the assertion is about the code the
    // user ends up with rather than about an offset.
    std::string code =
        "class C {\n"
        "    int get_X() { return 1; }\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range at{ {1, 4}, {1, 14} };
    auto actions = env.CodeActions(at, AccessorHintAt(at));

    const auto *fix = ActionTitled(actions, "Add the 'property' keyword");
    REQUIRE(fix != nullptr);
    const auto &edit = fix->edit->changes->begin()->second[0];

    // Rebuild the edited line.
    std::string line = "    int get_X() { return 1; }";
    line.insert(edit.range.start.character, edit.newText);

    CHECK(line == "    int get_X() property { return 1; }");
}

TEST_CASE("CodeActionHandler - An interface accessor is offered no keyword to add")
{
    // AngelScript's parser refuses `property` on an interface method outright - the oracle answers
    // "Expected ';'" - so a fix here would hand the user code that does not compile. The mode-3
    // rejection an interface like this causes lands on the IMPLEMENTING class's accessor, and that
    // is where both the hint and the fix belong.
    std::string code =
        "interface I {\n"
        "    int get_X();\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range at{ {1, 4}, {1, 14} };
    auto actions = env.CodeActions(at, AccessorHintAt(at));

    CHECK(ActionTitled(actions, "Add the 'property' keyword") == nullptr);
}

TEST_CASE("CodeActionHandler - No accessor fix without the hint")
{
    // Bound to the diagnostic. The hint is opt-in, and with it switched off no accessor in the file
    // should sprout an offer to rewrite it.
    std::string code =
        "class C {\n"
        "    int get_X() { return 1; }\n"
        "}\n";

    TestEnvironment env(code);
    const lsp::Range at{ {1, 4}, {1, 14} };
    auto actions = env.CodeActions(at, lsp::CodeActionContext{});

    CHECK(ActionTitled(actions, "Add the 'property' keyword") == nullptr);
}
