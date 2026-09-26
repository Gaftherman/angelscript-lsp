#include <doctest/doctest.h>

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/QueryRegistry.h"

#include <array>
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

using namespace angel_lsp;

TEST_CASE("QueryRegistry - Precompiled queries and thread-local cursor invariants")
{
    // Invariant 1: All precompiled queries are non-null
    const TSQuery* highlights = parser::QueryRegistry::GetHighlightsQuery();
    const TSQuery* locals = parser::QueryRegistry::GetLocalsQuery();
    const TSQuery* tags = parser::QueryRegistry::GetTagsQuery();
    const TSQuery* binaryExpr = parser::QueryRegistry::GetBinaryExpressionQuery();

    CHECK(highlights != nullptr);
    CHECK(locals != nullptr);
    CHECK(tags != nullptr);
    CHECK(binaryExpr != nullptr);

    // Invariant 2: Repeated calls return the exact same static pointer
    CHECK(parser::QueryRegistry::GetHighlightsQuery() == highlights);
    CHECK(parser::QueryRegistry::GetLocalsQuery() == locals);
    CHECK(parser::QueryRegistry::GetTagsQuery() == tags);
    CHECK(parser::QueryRegistry::GetBinaryExpressionQuery() == binaryExpr);

    // Invariant 3: Thread-local cursor is non-null and stable per thread
    TSQueryCursor* cursor1 = parser::QueryRegistry::GetThreadLocalCursor();
    TSQueryCursor* cursor2 = parser::QueryRegistry::GetThreadLocalCursor();
    CHECK(cursor1 != nullptr);
    CHECK(cursor1 == cursor2);

    // Invariant 4: Cursor execution on AST with randomized symbol
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

namespace {
/**
 * @brief Environment bundle passed to concurrent query worker threads.
 */
struct WorkerEnv
{
    const TSQuery* tags;
    std::vector<const TSQuery*>& threadTags;
    std::vector<TSQueryCursor*>& threadCursors;
    std::array<std::atomic<bool>, 6>& matches;
    std::latch& ready;
    std::latch& start;
    std::latch& done;
    std::latch& exit;
};

/**
 * @brief Executes AST query resolution on a dedicated worker thread.
 * @param[in] i Worker thread index.
 * @param[in,out] env Shared worker execution environment.
 */
void RunWorker(int i, WorkerEnv& env)
{
    env.threadTags[i] = parser::QueryRegistry::GetTagsQuery();
    env.threadCursors[i] = parser::QueryRegistry::GetThreadLocalCursor();
    env.ready.count_down();
    env.start.wait();

    const std::string sym = test::GenerateRandomSymbolName("WorkerFunc");
    parser::AngelScriptParser threadParser;
    TSTree* threadTree = threadParser.Parse("void " + sym + "(int x) { int y = x * 2; }\n");
    if (threadTree)
    {
        ts_query_cursor_exec(env.threadCursors[i], env.tags, ts_tree_root_node(threadTree));
        TSQueryMatch match;
        while (ts_query_cursor_next_match(env.threadCursors[i], &match))
        {
            if (match.capture_count > 0)
            {
                env.matches[i].store(true, std::memory_order_release);
                break;
            }
        }
        ts_tree_delete(threadTree);
    }
    env.done.count_down();
    env.exit.wait();
}
} // namespace

TEST_CASE("QueryRegistry - Concurrent threads receive distinct thread-local cursors")
{
    constexpr int kThreads = 6;
    const TSQuery* tags = parser::QueryRegistry::GetTagsQuery();
    std::vector<std::thread> workers;
    std::vector<const TSQuery*> threadTags(kThreads, nullptr);
    std::vector<TSQueryCursor*> threadCursors(kThreads, nullptr);
    std::array<std::atomic<bool>, kThreads> threadMatches{};

    std::latch readyLatch(kThreads);
    std::latch startLatch(1);
    std::latch doneLatch(kThreads);
    std::latch exitLatch(1);
    WorkerEnv env{tags, threadTags, threadCursors, threadMatches, readyLatch, startLatch, doneLatch, exitLatch};

    for (int i = 0; i < kThreads; ++i)
    {
        workers.emplace_back([i, &env]() { RunWorker(i, env); });
    }

    readyLatch.wait();
    for (int i = 0; i < kThreads; ++i)
    {
        CHECK(threadTags[i] == tags);
        REQUIRE(threadCursors[i] != nullptr);
        for (int j = i + 1; j < kThreads; ++j)
        {
            CHECK(threadCursors[i] != threadCursors[j]);
        }
    }

    startLatch.count_down();
    doneLatch.wait();
    exitLatch.count_down();

    for (auto& w : workers)
    {
        w.join();
    }
    for (int i = 0; i < kThreads; ++i)
    {
        CHECK(threadMatches[i].load(std::memory_order_acquire));
    }
}
