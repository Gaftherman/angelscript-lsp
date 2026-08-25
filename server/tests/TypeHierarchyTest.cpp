#include <doctest/doctest.h>

#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// Type hierarchy.
//
// Both directions answer *directly* rather than transitively: the client expands the tree one
// level at a time, and a transitive answer would list every ancestor again under each of its own
// descendants.
// =====================================================================================

namespace
{
    struct Fixture
    {
        AngelScriptParser parser;
        SymbolCollector collector{ nullptr };
        SymbolTable table;
        std::string uri = "file:///types.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        explicit Fixture(std::string code)
            : sourceCode(std::move(code))
        {
            tree = parser.Parse(sourceCode);
            collector.CollectSymbols(uri, sourceCode, parser, table);
        }

        ~Fixture()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<std::vector<lsp::TypeHierarchyItem>> Prepare(uint32_t line, uint32_t character)
        {
            const TypeHierarchyPrepareRequest request{
                uri, sourceCode, tree, table, lsp::Position{ line, character }
            };
            return PrepareTypeHierarchy(request);
        }

        std::optional<std::vector<lsp::TypeHierarchyItem>> Supertypes(const lsp::TypeHierarchyItem &item)
        {
            return GetSupertypes(TypeHierarchyItemRequest{ table, item });
        }

        std::optional<std::vector<lsp::TypeHierarchyItem>> Subtypes(const lsp::TypeHierarchyItem &item)
        {
            return GetSubtypes(TypeHierarchyItemRequest{ table, item });
        }
    };

    bool HasName(const std::optional<std::vector<lsp::TypeHierarchyItem>> &items, const std::string &name)
    {
        return items.has_value() &&
               std::any_of(items->begin(), items->end(), [&name](const lsp::TypeHierarchyItem &item)
               {
                   return item.name == name;
               });
    }
}

TEST_CASE("TypeHierarchy - Opens on the type name under the cursor")
{
    Fixture fixture(
        "class Base { }\n"
        "class Derived : Base { }\n");

    const auto items = fixture.Prepare(1, 8);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].name == "Derived");
    CHECK((*items)[0].kind == lsp::SymbolKind::Class);
}

TEST_CASE("TypeHierarchy - Opens on the type whose body the cursor sits in")
{
    // Asking for the hierarchy from inside a class is what a reader actually does; requiring the
    // cursor be parked on the name would make the feature feel broken.
    Fixture fixture(
        "class Base { }\n"
        "class Derived : Base\n"
        "{\n"
        "    int health;\n"
        "}\n");

    const auto items = fixture.Prepare(3, 9);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].name == "Derived");
}

TEST_CASE("TypeHierarchy - An interface opens as an interface")
{
    Fixture fixture("interface IThinker { void Think(); }\n");

    const auto items = fixture.Prepare(0, 12);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].kind == lsp::SymbolKind::Interface);
}

TEST_CASE("TypeHierarchy - Supertypes are the declared bases")
{
    Fixture fixture(
        "interface IThinker { void Think(); }\n"
        "class Base { }\n"
        "class Derived : Base, IThinker { void Think() { } }\n");

    const auto items = fixture.Prepare(2, 8);
    REQUIRE(items.has_value());

    const auto supertypes = fixture.Supertypes((*items)[0]);
    REQUIRE(supertypes.has_value());
    CHECK(HasName(supertypes, "Base"));
    CHECK(HasName(supertypes, "IThinker"));
}

TEST_CASE("TypeHierarchy - Supertypes stop at the direct bases")
{
    Fixture fixture(
        "class Root { }\n"
        "class Middle : Root { }\n"
        "class Leaf : Middle { }\n");

    const auto items = fixture.Prepare(2, 8);
    REQUIRE(items.has_value());

    const auto supertypes = fixture.Supertypes((*items)[0]);
    REQUIRE(supertypes.has_value());
    CHECK(supertypes->size() == 1);
    CHECK(HasName(supertypes, "Middle"));
    CHECK_FALSE(HasName(supertypes, "Root"));
}

TEST_CASE("TypeHierarchy - Subtypes stop at the direct ones")
{
    Fixture fixture(
        "class Root { }\n"
        "class Middle : Root { }\n"
        "class Leaf : Middle { }\n");

    const auto items = fixture.Prepare(0, 8);
    REQUIRE(items.has_value());

    const auto subtypes = fixture.Subtypes((*items)[0]);
    REQUIRE(subtypes.has_value());
    CHECK(subtypes->size() == 1);
    CHECK(HasName(subtypes, "Middle"));
    CHECK_FALSE(HasName(subtypes, "Leaf"));
}

TEST_CASE("TypeHierarchy - An interface's subtypes are its implementors and its heirs")
{
    Fixture fixture(
        "interface IBase { void Think(); }\n"
        "interface IMiddle : IBase { }\n"
        "class Leaf : IBase { void Think() { } }\n");

    const auto items = fixture.Prepare(0, 12);
    REQUIRE(items.has_value());

    const auto subtypes = fixture.Subtypes((*items)[0]);
    REQUIRE(subtypes.has_value());
    CHECK(HasName(subtypes, "IMiddle"));
    CHECK(HasName(subtypes, "Leaf"));
}

TEST_CASE("TypeHierarchy - A base that resolves to nothing is left out")
{
    // An engine-registered type has no declaration to navigate to, and an item pointing nowhere is
    // worse than an absent one.
    Fixture fixture("class Derived : CBaseEntity { }\n");

    const auto items = fixture.Prepare(0, 8);
    REQUIRE(items.has_value());
    CHECK_FALSE(fixture.Supertypes((*items)[0]).has_value());
}

TEST_CASE("TypeHierarchy - A type with no relations answers with nothing either way")
{
    Fixture fixture("class Lonely { }\n");

    const auto items = fixture.Prepare(0, 8);
    REQUIRE(items.has_value());
    CHECK_FALSE(fixture.Supertypes((*items)[0]).has_value());
    CHECK_FALSE(fixture.Subtypes((*items)[0]).has_value());
}

TEST_CASE("TypeHierarchy - A cursor on nothing type-shaped opens no hierarchy")
{
    Fixture fixture(
        "int g_count = 0;\n"
        "void Spawn() { }\n");

    CHECK_FALSE(fixture.Prepare(0, 5).has_value());
    CHECK_FALSE(fixture.Prepare(1, 6).has_value());
}

TEST_CASE("TypeHierarchy - The selection range is contained by the item's range")
{
    // The protocol's hard requirement, and the one a client will misbehave on.
    Fixture fixture(
        "class Derived : Base\n"
        "{\n"
        "    int health;\n"
        "}\n"
        "class Base { }\n");

    const auto items = fixture.Prepare(0, 8);
    REQUIRE(items.has_value());

    const auto &item = (*items)[0];
    const bool startsBefore = item.range.start.line < item.selectionRange.start.line ||
                              (item.range.start.line == item.selectionRange.start.line &&
                               item.range.start.character <= item.selectionRange.start.character);
    const bool endsAfter = item.range.end.line > item.selectionRange.end.line ||
                           (item.range.end.line == item.selectionRange.end.line &&
                            item.range.end.character >= item.selectionRange.end.character);
    CHECK(startsBefore);
    CHECK(endsAfter);
}
