#include <doctest/doctest.h>

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/QueryRegistry.h"

#include <thread>
#include <vector>

using namespace angel_lsp;

TEST_CASE("QueryRegistry - Precompiled queries and thread-local cursor invariants")
{
    // Invariant 1: All precompiled queries are non-null
    const TSQuery* highlights = parser::QueryRegistry::GetHighlightsQuery();
    const TSQuery* locals = parser::QueryRegistry::GetLocalsQuery();
    const TSQuery* tags = parser::QueryRegistry::GetTagsQuery();

    CHECK(highlights != nullptr);
    CHECK(locals != nullptr);
    CHECK(tags != nullptr);

    // Invariant 2: Repeated calls return the exact same static pointer
    CHECK(parser::QueryRegistry::GetHighlightsQuery() == highlights);
    CHECK(parser::QueryRegistry::GetLocalsQuery() == locals);
    CHECK(parser::QueryRegistry::GetTagsQuery() == tags);

    // Invariant 3: Thread-local cursor is non-null and stable per thread
    TSQueryCursor* cursor1 = parser::QueryRegistry::GetThreadLocalCursor();
    TSQueryCursor* cursor2 = parser::QueryRegistry::GetThreadLocalCursor();
    CHECK(cursor1 != nullptr);
    CHECK(cursor1 == cursor2);

    // Invariant 4: Concurrent threads receive identical static queries and distinct thread-local cursors
    constexpr int kThreads = 6;
    std::vector<std::thread> workers;
    std::vector<const TSQuery*> threadTags(kThreads, nullptr);
    std::vector<TSQueryCursor*> threadCursors(kThreads, nullptr);

    for (int i = 0; i < kThreads; ++i)
    {
        workers.emplace_back(
            [i, &threadTags, &threadCursors]()
            {
                threadTags[i] = parser::QueryRegistry::GetTagsQuery();
                threadCursors[i] = parser::QueryRegistry::GetThreadLocalCursor();
            });
    }

    for (auto& w : workers)
    {
        w.join();
    }

    for (int i = 0; i < kThreads; ++i)
    {
        CHECK(threadTags[i] == tags);
        CHECK(threadCursors[i] != nullptr);
        // Each worker thread must have its own distinct cursor instance
        for (int j = i + 1; j < kThreads; ++j)
        {
            CHECK(threadCursors[i] != threadCursors[j]);
        }
    }

    // Invariant 5: Cursor execution on AST with randomized symbol
    const std::string sym = test::GenerateRandomSymbolName("QueryFunc");
    const std::string code = "void " + sym + "(int a) { int local_val = 10; }\n";
    parser::AngelScriptParser parser;
    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    TSNode root = ts_tree_root_node(tree);
    ts_query_cursor_exec(cursor1, tags, root);

    TSQueryMatch match;
    bool foundMatch = false;
    while (ts_query_cursor_next_match(cursor1, &match))
    {
        if (match.capture_count > 0)
        {
            foundMatch = true;
            break;
        }
    }
    CHECK(foundMatch);

    ts_tree_delete(tree);
}
