#include <doctest/doctest.h>

#include "features/call_hierarchy/CallHierarchyHandler.h"
#include "analysis/CallGraph.h"
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
// Call hierarchy.
//
// The call graph is kept out of the SymbolTable on purpose - a call is not a declaration - so
// these tests build both and hand the handler each in its own right, the way the server does.
// =====================================================================================

namespace
{
    struct Fixture
    {
        AngelScriptParser parser;
        SymbolCollector collector{ nullptr };
        SymbolTable table;
        CallGraphIndex callGraph;
        std::string uri = "file:///calls.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        explicit Fixture(std::string code)
            : sourceCode(std::move(code))
        {
            tree = parser.Parse(sourceCode);
            collector.CollectSymbols(uri, sourceCode, parser, table);
            if (tree)
            {
                callGraph.SetDocumentCalls(uri, CollectCalls(ts_tree_root_node(tree), sourceCode));
            }
        }

        ~Fixture()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<std::vector<lsp::CallHierarchyItem>> Prepare(uint32_t line, uint32_t character)
        {
            const CallHierarchyPrepareRequest request{
                uri, sourceCode, tree, table, lsp::Position{ line, character }
            };
            return PrepareCallHierarchy(request);
        }

        std::optional<std::vector<lsp::CallHierarchyIncomingCall>> Incoming(const lsp::CallHierarchyItem &item)
        {
            return GetIncomingCalls(CallHierarchyItemRequest{ table, callGraph, item });
        }

        std::optional<std::vector<lsp::CallHierarchyOutgoingCall>> Outgoing(const lsp::CallHierarchyItem &item)
        {
            return GetOutgoingCalls(CallHierarchyItemRequest{ table, callGraph, item });
        }
    };

    bool HasFrom(const std::optional<std::vector<lsp::CallHierarchyIncomingCall>> &calls, const std::string &name)
    {
        return calls.has_value() &&
               std::any_of(calls->begin(), calls->end(), [&name](const lsp::CallHierarchyIncomingCall &call)
               {
                   return call.from.name == name;
               });
    }

    bool HasTo(const std::optional<std::vector<lsp::CallHierarchyOutgoingCall>> &calls, const std::string &name)
    {
        return calls.has_value() &&
               std::any_of(calls->begin(), calls->end(), [&name](const lsp::CallHierarchyOutgoingCall &call)
               {
                   return call.to.name == name;
               });
    }
}

// =====================================================================================
// The call graph itself
// =====================================================================================

TEST_CASE("CallGraph - Records the function each call is written inside")
{
    AngelScriptParser parser;
    const std::string code =
        "void Helper() { }\n"
        "void Spawn()\n"
        "{\n"
        "    Helper();\n"
        "}\n";

    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    const auto calls = CollectCalls(ts_tree_root_node(tree), code);
    ts_tree_delete(tree);

    REQUIRE(calls.size() == 1);
    CHECK(calls[0].caller == "Spawn");
    CHECK(calls[0].callee == "Helper");
    CHECK(calls[0].range.startLine == 3);
}

TEST_CASE("CallGraph - Qualifies a caller by its class and namespace")
{
    AngelScriptParser parser;
    const std::string code =
        "void Helper() { }\n"
        "namespace Game\n"
        "{\n"
        "    class Entity\n"
        "    {\n"
        "        void Think() { Helper(); }\n"
        "    }\n"
        "}\n";

    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    const auto calls = CollectCalls(ts_tree_root_node(tree), code);
    ts_tree_delete(tree);

    REQUIRE(calls.size() == 1);
    CHECK(calls[0].caller == "Game::Entity::Think");
}

TEST_CASE("CallGraph - A method call records the member name, not the receiver")
{
    AngelScriptParser parser;
    const std::string code =
        "class Entity { void Think() { } }\n"
        "void Spawn(Entity@ e) { e.Think(); }\n";

    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    const auto calls = CollectCalls(ts_tree_root_node(tree), code);
    ts_tree_delete(tree);

    REQUIRE(calls.size() == 1);
    CHECK(calls[0].callee == "Think");
}

TEST_CASE("CallGraph - A call nested in an argument is recorded too")
{
    AngelScriptParser parser;
    const std::string code =
        "int Inner() { return 1; }\n"
        "void Outer(int v) { }\n"
        "void Spawn() { Outer(Inner()); }\n";

    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    const auto calls = CollectCalls(ts_tree_root_node(tree), code);
    ts_tree_delete(tree);

    CHECK(calls.size() == 2);
}

TEST_CASE("CallGraph - A call written outside any function has no caller")
{
    AngelScriptParser parser;
    const std::string code =
        "int Compute() { return 1; }\n"
        "int g_value = Compute();\n";

    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    const auto calls = CollectCalls(ts_tree_root_node(tree), code);
    ts_tree_delete(tree);

    REQUIRE(calls.size() == 1);
    CHECK(calls[0].caller.empty());
}

TEST_CASE("CallGraph - Clearing a document removes its calls")
{
    CallGraphIndex index;
    index.SetDocumentCalls("file:///a.as", { CallSite{ "Spawn", "Helper", {} } });
    CHECK(index.FindCallsTo("Helper").size() == 1);

    index.ClearDocument("file:///a.as");
    CHECK(index.FindCallsTo("Helper").empty());
}

// =====================================================================================
// The hierarchy
// =====================================================================================

TEST_CASE("CallHierarchy - Opens on the function name under the cursor")
{
    Fixture fixture(
        "void Helper() { }\n"
        "void Spawn() { Helper(); }\n");

    const auto items = fixture.Prepare(0, 6);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].name == "Helper");
    CHECK((*items)[0].kind == lsp::SymbolKind::Function);
}

TEST_CASE("CallHierarchy - Opens on the function whose body the cursor sits in")
{
    Fixture fixture(
        "void Helper() { }\n"
        "void Spawn()\n"
        "{\n"
        "    int ticks = 0;\n"
        "}\n");

    const auto items = fixture.Prepare(3, 9);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].name == "Spawn");
}

TEST_CASE("CallHierarchy - A method opens as a method and carries its qualified name")
{
    Fixture fixture("class Entity { void Think() { } }\n");

    const auto items = fixture.Prepare(0, 20);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].kind == lsp::SymbolKind::Method);
    REQUIRE((*items)[0].detail.has_value());
    CHECK((*items)[0].detail.value() == "Entity::Think");
}

TEST_CASE("CallHierarchy - Incoming calls name the calling function")
{
    Fixture fixture(
        "void Helper() { }\n"
        "void Spawn() { Helper(); }\n"
        "void Respawn() { Helper(); }\n");

    const auto items = fixture.Prepare(0, 6);
    REQUIRE(items.has_value());

    const auto incoming = fixture.Incoming((*items)[0]);
    REQUIRE(incoming.has_value());
    CHECK(incoming->size() == 2);
    CHECK(HasFrom(incoming, "Spawn"));
    CHECK(HasFrom(incoming, "Respawn"));
}

TEST_CASE("CallHierarchy - One caller with two calls is one entry with two ranges")
{
    // The protocol asks for one entry per caller carrying every range at which it writes the call.
    Fixture fixture(
        "void Helper() { }\n"
        "void Spawn()\n"
        "{\n"
        "    Helper();\n"
        "    Helper();\n"
        "}\n");

    const auto items = fixture.Prepare(0, 6);
    REQUIRE(items.has_value());

    const auto incoming = fixture.Incoming((*items)[0]);
    REQUIRE(incoming.has_value());
    REQUIRE(incoming->size() == 1);
    CHECK((*incoming)[0].fromRanges.size() == 2);
}

TEST_CASE("CallHierarchy - A call written outside any function yields no caller entry")
{
    Fixture fixture(
        "int Compute() { return 1; }\n"
        "int g_value = Compute();\n");

    const auto items = fixture.Prepare(0, 5);
    REQUIRE(items.has_value());
    CHECK_FALSE(fixture.Incoming((*items)[0]).has_value());
}

TEST_CASE("CallHierarchy - Outgoing calls name what the function calls")
{
    Fixture fixture(
        "void First() { }\n"
        "void Second() { }\n"
        "void Spawn()\n"
        "{\n"
        "    First();\n"
        "    Second();\n"
        "}\n");

    const auto items = fixture.Prepare(2, 6);
    REQUIRE(items.has_value());

    const auto outgoing = fixture.Outgoing((*items)[0]);
    REQUIRE(outgoing.has_value());
    CHECK(outgoing->size() == 2);
    CHECK(HasTo(outgoing, "First"));
    CHECK(HasTo(outgoing, "Second"));
}

TEST_CASE("CallHierarchy - Outgoing calls reach a method through its receiver")
{
    Fixture fixture(
        "class Entity { void Think() { } }\n"
        "void Spawn(Entity@ e) { e.Think(); }\n");

    const auto items = fixture.Prepare(1, 6);
    REQUIRE(items.has_value());

    const auto outgoing = fixture.Outgoing((*items)[0]);
    REQUIRE(outgoing.has_value());
    CHECK(HasTo(outgoing, "Think"));
}

TEST_CASE("CallHierarchy - A callee with no declaration here is left out")
{
    // An engine-registered function has nowhere to navigate to, so it is not offered as an entry.
    Fixture fixture("void Spawn() { g_EngineFuncs.ServerPrint(\"hi\"); }\n");

    const auto items = fixture.Prepare(0, 6);
    REQUIRE(items.has_value());
    CHECK_FALSE(fixture.Outgoing((*items)[0]).has_value());
}

TEST_CASE("CallHierarchy - A function that calls nothing and is called by nothing answers with nothing")
{
    Fixture fixture("void Lonely() { int ticks = 0; }\n");

    const auto items = fixture.Prepare(0, 6);
    REQUIRE(items.has_value());
    CHECK_FALSE(fixture.Incoming((*items)[0]).has_value());
    CHECK_FALSE(fixture.Outgoing((*items)[0]).has_value());
}

TEST_CASE("CallHierarchy - A cursor on nothing callable opens no hierarchy")
{
    Fixture fixture(
        "int g_count = 0;\n"
        "class Entity { }\n");

    CHECK_FALSE(fixture.Prepare(0, 5).has_value());
}
