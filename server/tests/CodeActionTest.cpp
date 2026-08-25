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

