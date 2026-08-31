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
