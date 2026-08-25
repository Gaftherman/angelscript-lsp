#include <doctest/doctest.h>

#include "features/inlay_hint/InlayHintHandler.h"
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

        std::optional<std::vector<lsp::InlayHint>> InlayHints(lsp::Range range = lsp::Range{ {0, 0}, {0, 0} })
        {
            InlayHintRequest req{ uri, sourceCode, tree, range, symbolTable, scopeIndex };
            return GetInlayHints(req);
        }
    };
}

TEST_CASE("InlayHintHandler - Basic Function Call Parameter Hints")
{
    std::string code =
        "void Test(int a, float b) {}\n"
        "void main() {\n"
        "    Test(10, 2.5f);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() >= 2);

    // First parameter hint: a:
    std::string label0 = std::holds_alternative<std::string>(hints->at(0).label)
                             ? std::get<std::string>(hints->at(0).label)
                             : "";
    CHECK(label0 == "a:");
    CHECK(hints->at(0).kind.has_value());
    bool isParam0 = (hints->at(0).kind.value() == lsp::InlayHintKind::Parameter);
    CHECK(isParam0);

    // Second parameter hint: b:
    std::string label1 = std::holds_alternative<std::string>(hints->at(1).label)
                             ? std::get<std::string>(hints->at(1).label)
                             : "";
    CHECK(label1 == "b:");
    CHECK(hints->at(1).kind.has_value());
    bool isParam1 = (hints->at(1).kind.value() == lsp::InlayHintKind::Parameter);
    CHECK(isParam1);
}

TEST_CASE("InlayHintHandler - Exclusion Rule: Named Arguments")
{
    std::string code =
        "void SetValues(int x, int y) {}\n"
        "void main() {\n"
        "    SetValues(x: 10, 20);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    // Only 'y:' should be emitted since 'x' is already explicitly named in syntax
    REQUIRE(hints->size() == 1);
    std::string label = std::holds_alternative<std::string>(hints->at(0).label)
                            ? std::get<std::string>(hints->at(0).label)
                            : "";
    CHECK(label == "y:");
}

TEST_CASE("InlayHintHandler - Exclusion Rule: Same-Name Arguments")
{
    std::string code =
        "void SetDimensions(int width, int height) {}\n"
        "void main() {\n"
        "    int width = 100;\n"
        "    int h = 200;\n"
        "    SetDimensions(width, h);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    // 'width' matches parameter name 'width', so it must be suppressed.
    // 'h' does not match 'height', so 'height:' should be emitted.
    bool foundWidthHint = false;
    bool foundHeightHint = false;

    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        if (l == "width:")
        {
            foundWidthHint = true;
        }
        if (l == "height:")
        {
            foundHeightHint = true;
        }
    }

    CHECK(!foundWidthHint);
    CHECK(foundHeightHint);
}

TEST_CASE("InlayHintHandler - Class Method Call with Inheritance")
{
    std::string code =
        "class Base {\n"
        "    void Attack(int damage, float radius) {}\n"
        "}\n"
        "class Player : Base {}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.Attack(50, 10.0f);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    bool foundDamage = false;
    bool foundRadius = false;

    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        if (l == "damage:")
        {
            foundDamage = true;
        }
        if (l == "radius:")
        {
            foundRadius = true;
        }
    }

    CHECK(foundDamage);
    CHECK(foundRadius);
}

TEST_CASE("InlayHintHandler - Auto Variable Type Deduction for Literals")
{
    std::string code =
        "void main() {\n"
        "    auto a = 42;\n"
        "    auto b = 3.14f;\n"
        "    auto c = 2.718;\n"
        "    auto d = true;\n"
        "    auto e = \"hello\";\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 5);

    std::vector<std::string> labels;
    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        labels.push_back(l);
        CHECK(hint.kind.has_value());
        bool isType = (hint.kind.value() == lsp::InlayHintKind::Type);
        CHECK(isType);
    }

    CHECK(labels[0] == ": int");
    CHECK(labels[1] == ": float");
    CHECK(labels[2] == ": double");
    CHECK(labels[3] == ": bool");
    CHECK(labels[4] == ": string");
}

TEST_CASE("InlayHintHandler - Auto Variable Deduction for Function Calls and Casts")
{
    std::string code =
        "class Actor {}\n"
        "Actor@ SpawnActor() { return null; }\n"
        "interface IWeapon {}\n"
        "void main() {\n"
        "    auto actor = SpawnActor();\n"
        "    auto weapon = cast<IWeapon>(null);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() >= 2);

    bool foundActorType = false;
    bool foundWeaponType = false;

    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        if (l == ": Actor@")
        {
            foundActorType = true;
        }
        if (l == ": IWeapon" || l == ": IWeapon@")
        {
            foundWeaponType = true;
        }
    }

    CHECK(foundActorType);
    CHECK(foundWeaponType);
}

TEST_CASE("InlayHintHandler - Sub-range Filtering")
{
    std::string code =
        "void Foo(int x) {}\n"
        "void Bar(int y) {}\n"
        "void main() {\n"
        "    Foo(1);\n"
        "    Bar(2);\n"
        "}\n";

    TestEnvironment env(code);

    // Range restricting to only line 3 (Foo(1))
    lsp::Range r{ {3, 0}, {3, 20} };
    auto hints = env.InlayHints(r);

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 1);
    std::string l = std::holds_alternative<std::string>(hints->at(0).label)
                        ? std::get<std::string>(hints->at(0).label)
                        : "";
    CHECK(l == "x:");
}

TEST_CASE("InlayHintHandler - Operator Overload Auto Type Deduction")
{
    std::string code =
        "class Matrix {\n"
        "    Matrix opMul(float scalar) const { return Matrix(); }\n"
        "}\n"
        "class Vector {\n"
        "    Vector opMul_r(const Matrix &in m) const { return Vector(); }\n"
        "}\n"
        "void Main() {\n"
        "    Matrix m;\n"
        "    Vector v;\n"
        "    auto res1 = m * 2.0f;\n"
        "    auto res2 = m * v;\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    bool foundMatrix = false;
    bool foundVector = false;

    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        if (l == ": Matrix")
        {
            foundMatrix = true;
            if (hint.tooltip.has_value())
            {
                std::string t = std::holds_alternative<std::string>(*hint.tooltip)
                                    ? std::get<std::string>(*hint.tooltip)
                                    : "";
                CHECK(t == "Deduced type: Matrix");
            }
        }
        if (l == ": Vector")
        {
            foundVector = true;
            if (hint.tooltip.has_value())
            {
                std::string t = std::holds_alternative<std::string>(*hint.tooltip)
                                    ? std::get<std::string>(*hint.tooltip)
                                    : "";
                CHECK(t == "Deduced type: Vector");
            }
        }
    }

    CHECK(foundMatrix);
    CHECK(foundVector);
}

TEST_CASE("InlayHintHandler - Robustness with Empty / Null Tree")
{
    InlayHintRequest req{ "file:///empty.as", "", nullptr, lsp::Range{}, SymbolTable{}, ScopeIndex{} };
    auto hints = GetInlayHints(req);
    CHECK(!hints.has_value());
}
