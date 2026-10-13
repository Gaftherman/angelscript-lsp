#include <doctest/doctest.h>

#include "document/Document.h"
#include "helpers/TestUtils.h"
#include "lsp/DocumentStore.h"
#include "parser/AngelScriptParser.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace angel_lsp;

/**
 * @brief Generates a randomized AngelScript code snippet.
 */
std::string GenerateRandomCode(std::mt19937_64& rng)
{
    const std::string className = test::GenerateRandomSymbolName("Cls");
    const std::string funcName = test::GenerateRandomSymbolName("fn");
    const std::string varName = test::GenerateRandomSymbolName("m_val");

    std::uniform_int_distribution<int> valDist(1, 10000);
    return "class " + className +
           " {\n"
           "    int " +
           varName + " = " + std::to_string(valDist(rng)) +
           ";\n"
           "    void " +
           funcName +
           "() {\n"
           "        " +
           varName +
           " += 42;\n"
           "    }\n"
           "};\n";
}

struct WriterContext
{
    DocumentStore& store;
    std::string uri;
    std::latch& startLatch;
    std::latch& readersActiveLatch;
    std::atomic<bool>& stopFlag;
    std::atomic<uint32_t>& totalWrites;
    uint32_t targetWrites = 1000;
};

struct ReaderContext
{
    const DocumentStore& store;
    std::string uri;
    std::latch& startLatch;
    std::latch& readersActiveLatch;
    std::atomic<bool>& stopFlag;
    std::atomic<uint32_t>& successfulReads;
    std::atomic<uint32_t>& failedReads;
    uint32_t targetReads = 200;
};

/**
 * @brief Writer worker routine that rapidly updates DocumentStore with parsed ASTs.
 */
void RunWriterWorker(WriterContext ctx)
{
    std::mt19937_64 rng(std::random_device{}());
    parser::AngelScriptParser parser;
    const uint32_t maxWrites = ctx.targetWrites * 4;

    ctx.startLatch.arrive_and_wait();

    while (!ctx.stopFlag.load(std::memory_order_acquire))
    {
        const uint32_t prevWrites = ctx.totalWrites.fetch_add(1, std::memory_order_relaxed);
        if (prevWrites >= ctx.targetWrites && (ctx.readersActiveLatch.try_wait() || prevWrites >= maxWrites))
        {
            break;
        }
        std::string code = GenerateRandomCode(rng);
        TSTree* rawTree = parser.Parse(code);
        ctx.store.UpdateDocument(ctx.uri, std::move(code), 1, document::MakeTreePtr(rawTree));
    }
}

/**
 * @brief Reader worker routine that snapshots documents, copies ASTs, and inspects nodes.
 */
void RunReaderWorker(ReaderContext ctx)
{
    bool countedDown = false;
    ctx.startLatch.arrive_and_wait();

    while (!ctx.stopFlag.load(std::memory_order_acquire) ||
           ctx.successfulReads.load(std::memory_order_relaxed) < ctx.targetReads)
    {
        auto doc = ctx.store.GetDocument(ctx.uri);
        if (!doc || !doc->tree)
        {
            continue;
        }
        TSTree* copied = ts_tree_copy(doc->tree.get());
        if (!copied)
        {
            continue;
        }
        auto managedCopy = document::MakeTreePtr(copied);
        TSNode root = ts_tree_root_node(managedCopy.get());
        const char* type = ts_node_type(root);
        if (type == nullptr)
        {
            ctx.failedReads.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            ctx.successfulReads.fetch_add(1, std::memory_order_relaxed);
            if (!countedDown)
            {
                countedDown = true;
                ctx.readersActiveLatch.count_down();
            }
        }
    }
}
} // namespace

TEST_CASE("Concurrency - SEC-04 Randomized Concurrent Mutation vs AST Analysis Invariant")
{
    DocumentStore store;
    const std::string uri = "file:///sandbox/" + test::GenerateRandomSymbolName("doc") + ".as";

    std::mt19937_64 initRng(1337);
    std::string initialCode = GenerateRandomCode(initRng);
    parser::AngelScriptParser initParser;
    TSTree* initTree = initParser.Parse(initialCode);
    store.OpenDocument(uri, initialCode, 0, document::MakeTreePtr(initTree));

    constexpr int k_numWriters = 10;
    constexpr int k_numReaders = 5;
    constexpr uint32_t k_targetWrites = 1000;
    constexpr uint32_t k_targetReads = 200;

    std::latch startLatch(k_numWriters + k_numReaders);
    std::latch readersActiveLatch(k_numReaders);
    std::atomic<bool> stopFlag{false};
    std::atomic<uint32_t> totalWrites{0};
    std::atomic<uint32_t> successfulReads{0};
    std::atomic<uint32_t> failedReads{0};

    std::vector<std::thread> readers;
    readers.reserve(k_numReaders);
    for (int i = 0; i < k_numReaders; ++i)
    {
        readers.emplace_back(RunReaderWorker, ReaderContext{store, uri, startLatch, readersActiveLatch, stopFlag,
                                                            successfulReads, failedReads, k_targetReads});
    }

    std::vector<std::thread> writers;
    writers.reserve(k_numWriters);
    for (int i = 0; i < k_numWriters; ++i)
    {
        writers.emplace_back(RunWriterWorker, WriterContext{store, uri, startLatch, readersActiveLatch, stopFlag,
                                                            totalWrites, k_targetWrites});
    }

    for (auto& w : writers)
    {
        if (w.joinable())
            w.join();
    }

    stopFlag.store(true, std::memory_order_release);

    for (auto& r : readers)
    {
        if (r.joinable())
            r.join();
    }

    CHECK(totalWrites.load() >= k_targetWrites);
    CHECK(successfulReads.load() >= k_targetReads);
    CHECK(failedReads.load() == 0);
}
