#include <doctest/doctest.h>

#include "document/Document.h"
#include "helpers/TestUtils.h"
#include "lsp/DocumentStore.h"
#include "parser/AngelScriptParser.h"

#include <atomic>
#include <chrono>
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
    std::atomic<bool>& stopFlag;
    std::atomic<uint32_t>& totalWrites;
    uint32_t targetWrites = 1000;
};

/**
 * @brief Writer worker routine that rapidly updates DocumentStore with parsed ASTs.
 */
void RunWriterWorker(WriterContext ctx)
{
    std::mt19937_64 rng(std::random_device{}());
    parser::AngelScriptParser parser;

    while (!ctx.stopFlag.load(std::memory_order_relaxed) &&
           ctx.totalWrites.fetch_add(1, std::memory_order_relaxed) < ctx.targetWrites)
    {
        std::string code = GenerateRandomCode(rng);
        TSTree* rawTree = parser.Parse(code);
        ctx.store.UpdateDocument(ctx.uri, std::move(code), 1, document::MakeTreePtr(rawTree));
    }
}

/**
 * @brief Reader worker routine that snapshots documents, copies ASTs, and inspects nodes.
 */
void RunReaderWorker(const DocumentStore& store, const std::string& uri, std::atomic<bool>& stopFlag,
                     std::atomic<uint32_t>& successfulReads)
{
    while (!stopFlag.load(std::memory_order_relaxed))
    {
        auto doc = store.GetDocument(uri);
        if (doc && doc->tree)
        {
            TSTree* copied = ts_tree_copy(doc->tree.get());
            if (copied)
            {
                auto managedCopy = document::MakeTreePtr(copied);
                TSNode root = ts_tree_root_node(managedCopy.get());
                const char* type = ts_node_type(root);
                CHECK(type != nullptr);
                successfulReads.fetch_add(1, std::memory_order_relaxed);
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

    std::atomic<bool> stopFlag{false};
    std::atomic<uint32_t> totalWrites{0};
    std::atomic<uint32_t> successfulReads{0};
    constexpr uint32_t k_targetWrites = 1000;

    std::vector<std::thread> writers;
    writers.reserve(10);
    for (int i = 0; i < 10; ++i)
    {
        writers.emplace_back(RunWriterWorker, WriterContext{store, uri, stopFlag, totalWrites, k_targetWrites});
    }

    std::vector<std::thread> readers;
    readers.reserve(5);
    for (int i = 0; i < 5; ++i)
    {
        readers.emplace_back(RunReaderWorker, std::cref(store), uri, std::ref(stopFlag), std::ref(successfulReads));
    }

    for (auto& w : writers)
    {
        if (w.joinable())
            w.join();
    }

    stopFlag.store(true, std::memory_order_relaxed);

    for (auto& r : readers)
    {
        if (r.joinable())
            r.join();
    }

    CHECK(totalWrites.load() >= k_targetWrites);
    CHECK(successfulReads.load() > 0);
}
