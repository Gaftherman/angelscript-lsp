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

TEST_CASE("TypeHierarchy - Type declared inside a namespace opens on declaration")
{
    Fixture fixture(
        "namespace Game\n"
        "{\n"
        "    class Base { }\n"
        "    class Player : Base { }\n"
        "}\n");

    // Line 3: "    class Player : Base { }" -> col 12 is on Player
    const auto items = fixture.Prepare(3, 12);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].name == "Player");
    CHECK((*items)[0].kind == lsp::SymbolKind::Class);

    const auto supertypes = fixture.Supertypes((*items)[0]);
    REQUIRE(supertypes.has_value());
    CHECK(HasName(supertypes, "Base"));
}

TEST_CASE("TypeHierarchy - Type declared inside nested namespaces opens inside body")
{
    Fixture fixture(
        "namespace Outer\n"
        "{\n"
        "    namespace Inner\n"
        "    {\n"
        "        class Widget\n"
        "        {\n"
        "            void Update() { }\n"
        "        }\n"
        "    }\n"
        "}\n");

    // Line 6: "            void Update() { }" -> col 18 is inside Widget body
    const auto items = fixture.Prepare(6, 18);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].name == "Widget");
}

TEST_CASE("TypeHierarchy - Type inside namespace resolved by short name fallback")
{
    Fixture fixture(
        "namespace Library\n"
        "{\n"
        "    class Service { }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Service s;\n"
        "}\n");

    // Line 6: "    Service s;" -> col 6 is on Service
    const auto items = fixture.Prepare(6, 6);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].name == "Service");
}

TEST_CASE("TypeHierarchy - Supertypes resolves qualified base when same-named global type exists")
{
    Fixture fixture(
        "class Base { }\n"
        "namespace Other\n"
        "{\n"
        "    class Base { }\n"
        "}\n"
        "class Derived : Other::Base { }\n");

    // Line 5: "class Derived : Other::Base { }" -> col 8 is on Derived
    const auto items = fixture.Prepare(5, 8);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);

    const auto supertypes = fixture.Supertypes((*items)[0]);
    REQUIRE(supertypes.has_value());
    REQUIRE(supertypes->size() == 1);
    CHECK((*supertypes)[0].name == "Base");
    // Line 3 is Other::Base; line 0 is global Base. Must resolve to Other::Base (line 3).
    CHECK((*supertypes)[0].range.start.line == 3);
}

TEST_CASE("TypeHierarchy - Prepare on scoped identifier opens qualified type")
{
    Fixture fixture(
        "namespace Outer\n"
        "{\n"
        "    class Widget { }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Outer::Widget w;\n"
        "}\n");

    // Line 6: "    Outer::Widget w;" -> col 5 is on Outer, col 12 is on Widget
    const auto itemsOuter = fixture.Prepare(6, 5);
    REQUIRE(itemsOuter.has_value());
    REQUIRE(itemsOuter->size() == 1);
    CHECK((*itemsOuter)[0].name == "Widget");
    CHECK((*itemsOuter)[0].range.start.line == 2);

    const auto itemsWidget = fixture.Prepare(6, 12);
    REQUIRE(itemsWidget.has_value());
    REQUIRE(itemsWidget->size() == 1);
    CHECK((*itemsWidget)[0].name == "Widget");
    CHECK((*itemsWidget)[0].range.start.line == 2);
}

TEST_CASE("TypeHierarchy - Supertypes isolates bases of same-named types across namespaces")
{
    Fixture fixture(
        "namespace NS1\n"
        "{\n"
        "    class Base1 { }\n"
        "    class Target : Base1 { }\n"
        "}\n"
        "namespace NS2\n"
        "{\n"
        "    class Base2 { }\n"
        "    class Target : Base2 { }\n"
        "}\n");

    // Line 3: NS1::Target
    const auto items1 = fixture.Prepare(3, 12);
    REQUIRE(items1.has_value());
    REQUIRE(items1->size() == 1);
    const auto supertypes1 = fixture.Supertypes((*items1)[0]);
    REQUIRE(supertypes1.has_value());
    CHECK(supertypes1->size() == 1);
    CHECK((*supertypes1)[0].name == "Base1");

    // Line 8: NS2::Target
    const auto items2 = fixture.Prepare(8, 12);
    REQUIRE(items2.has_value());
    REQUIRE(items2->size() == 1);
    const auto supertypes2 = fixture.Supertypes((*items2)[0]);
    REQUIRE(supertypes2.has_value());
    CHECK(supertypes2->size() == 1);
    CHECK((*supertypes2)[0].name == "Base2");
}
