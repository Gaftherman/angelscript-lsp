#include <doctest/doctest.h>

#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include "analysis/TargetResolution.h"
#include "document/Document.h"
#include "features/document_highlight/DocumentHighlightHandler.h"
#include "helpers/TestUtils.h"
#include "lsp/AnalysisScheduler.h"
#include "lsp/DocumentStore.h"
#include "parser/AngelScriptParser.h"
#include "utils/MultiFileLogger.h"

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace angel_lsp;

/**
 * @brief Context passed to writer worker threads.
 */
struct WriterStressContext
{
    DocumentStore& store;
    const std::vector<std::string>& uris;
    std::latch& startLatch;
    std::latch& readersActiveLatch;
    std::atomic<bool>& stopFlag;
    std::atomic<uint32_t>& totalWrites;
    uint32_t targetWrites = 1000;
};

/**
 * @brief Context passed to reader worker threads.
 */
struct ReaderStressContext
{
    const DocumentStore& store;
    const std::vector<std::string>& uris;
    std::latch& startLatch;
    std::latch& readersActiveLatch;
    std::atomic<bool>& stopFlag;
    std::atomic<uint32_t>& successReads;
    std::atomic<uint32_t>& failedReads;
    uint32_t targetReads = 200;
};

/**
 * @brief Generates randomized valid AngelScript class definition.
 * @param[in,out] rng Random number generator.
 * @return Source code snippet string.
 */
std::string GenerateRandomStressCode(std::mt19937_64& rng)
{
    const std::string cls = test::GenerateRandomSymbolName("Cls");
    const std::string fn = test::GenerateRandomSymbolName("fn");
    const std::string var = test::GenerateRandomSymbolName("v");
    std::uniform_int_distribution<int> valDist(1, 100000);

    return "class " + cls +
           " {\n"
           "    int " +
           var + " = " + std::to_string(valDist(rng)) +
           ";\n"
           "    void " +
           fn + "() { " + var +
           " += 1; }\n"
           "};\n";
}

/**
 * @brief Executes rapid document updates and closes in background.
 * @param[in] ctx Stress context containing store and sync primitives.
 */
void ExecuteWriterStress(WriterStressContext ctx)
{
    std::mt19937_64 rng(std::random_device{}());
    parser::AngelScriptParser parser;
    std::uniform_int_distribution<size_t> uriDist(0, ctx.uris.size() - 1);
    const uint32_t maxWrites = ctx.targetWrites * 4;

    ctx.startLatch.arrive_and_wait();

    while (!ctx.stopFlag.load(std::memory_order_acquire))
    {
        const uint32_t prevWrites = ctx.totalWrites.fetch_add(1, std::memory_order_relaxed);
        if (prevWrites >= ctx.targetWrites && (ctx.readersActiveLatch.try_wait() || prevWrites >= maxWrites))
        {
            break;
        }
        const std::string& uri = ctx.uris[uriDist(rng)];
        std::string code = GenerateRandomStressCode(rng);
        TSTree* rawTree = parser.Parse(code);
        ctx.store.UpdateDocument(uri, std::move(code), 1, document::MakeTreePtr(rawTree));
    }
}

/**
 * @brief Executes concurrent document reads and thread-safe AST copies.
 * @param[in] ctx Stress context containing store, URIs, and synchronization primitives.
 */
void ExecuteReaderStress(ReaderStressContext ctx)
{
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> uriDist(0, ctx.uris.size() - 1);
    bool countedDown = false;

    ctx.startLatch.arrive_and_wait();

    while (!ctx.stopFlag.load(std::memory_order_acquire) ||
           ctx.successReads.load(std::memory_order_relaxed) < ctx.targetReads)
    {
        const std::string& uri = ctx.uris[uriDist(rng)];
        auto docHandle = ctx.store.GetDocument(uri);
        if (!docHandle || !docHandle->tree)
        {
            continue;
        }
        TSTree* copied = ts_tree_copy(docHandle->tree.get());
        if (copied == nullptr)
        {
            continue;
        }
        auto managed = document::MakeTreePtr(copied);
        TSNode root = ts_tree_root_node(managed.get());
        if (!ts_node_is_null(root))
        {
            const char* type = ts_node_type(root);
            if (type == nullptr)
            {
                ctx.failedReads.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                ctx.successReads.fetch_add(1, std::memory_order_relaxed);
                if (!countedDown)
                {
                    countedDown = true;
                    ctx.readersActiveLatch.count_down();
                }
            }
        }
    }
}

/**
 * @brief Tests AST resilience against malformed and corrupted source snippets.
 * @param[in] malformedCode Erroneous source string.
 * @param[in] queryLine Line coordinate to test query handlers.
 * @param[in] queryCol Column coordinate to test query handlers.
 */
void AssertMalformedSnippetTolerance(const std::string& malformedCode, uint32_t queryLine, uint32_t queryCol)
{
    parser::AngelScriptParser parser;
    TSTree* rawTree = parser.Parse(malformedCode);
    REQUIRE(rawTree != nullptr);
    auto managedTree = document::MakeTreePtr(rawTree);

    analysis::SymbolTable symTable;
    analysis::ScopeIndex scopeIndex;
    const std::string uri = "file:///fuzzed_" + test::GenerateRandomSymbolName() + ".as";

    features::DocumentHighlightRequest hlReq{
        .uri = uri,
        .sourceCode = malformedCode,
        .tree = managedTree.get(),
        .position = lsp::Position{queryLine, queryCol},
        .symbolTable = symTable,
        .scopeIndex = scopeIndex,
    };
    auto hlResult = features::GetDocumentHighlights(hlReq);
    CHECK((!hlResult.has_value() || hlResult->empty()));

    TSNode outNode{};
    analysis::ResolveTargetRequest resReq{
        .uri = uri,
        .sourceCode = malformedCode,
        .tree = managedTree.get(),
        .position = analysis::TargetPosition{queryLine, queryCol},
        .symbolTable = symTable,
        .scopeIndex = scopeIndex,
        .outNode = outNode,
        .logger = nullptr,
    };
    auto resResult = analysis::ResolveTargetSymbol(resReq);
    CHECK(!resResult.has_value());
}

} // namespace

TEST_CASE("ConcurrencySafety - Simultaneous Document Mutation and AST Reading Invariant")
{
    DocumentStore store;
    std::vector<std::string> uris;
    uris.reserve(3);
    parser::AngelScriptParser initParser;

    for (int i = 0; i < 3; ++i)
    {
        std::string uri = "file:///stress_" + test::GenerateRandomSymbolName("doc") + ".as";
        std::string initCode = "int g_val = " + std::to_string(i) + ";\n";
        TSTree* initTree = initParser.Parse(initCode);
        store.OpenDocument(uri, initCode, 0, document::MakeTreePtr(initTree));
        uris.push_back(std::move(uri));
    }

    constexpr int k_numWriters = 2;
    constexpr int k_numReaders = 4;
    constexpr uint32_t k_targetWrites = 1000;
    constexpr uint32_t k_targetReads = 200;

    std::latch startLatch(k_numWriters + k_numReaders);
    std::latch readersActiveLatch(k_numReaders);
    std::atomic<bool> stopFlag{false};
    std::atomic<uint32_t> totalWrites{0};
    std::atomic<uint32_t> successReads{0};
    std::atomic<uint32_t> failedReads{0};

    std::vector<std::thread> readers;
    readers.reserve(k_numReaders);
    for (int i = 0; i < k_numReaders; ++i)
    {
        readers.emplace_back(ExecuteReaderStress,
                             ReaderStressContext{store, uris, startLatch, readersActiveLatch, stopFlag, successReads,
                                                 failedReads, k_targetReads});
    }

    std::vector<std::thread> writers;
    writers.reserve(k_numWriters);
    for (int i = 0; i < k_numWriters; ++i)
    {
        writers.emplace_back(ExecuteWriterStress, WriterStressContext{store, uris, startLatch, readersActiveLatch,
                                                                      stopFlag, totalWrites, k_targetWrites});
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
    CHECK(successReads.load() >= k_targetReads);
    CHECK(failedReads.load() == 0);
}

TEST_CASE("ConcurrencySafety - Secondary Thread Exception Containment Invariant")
{
    std::promise<void> thrownPromise;
    auto thrownFuture = thrownPromise.get_future();
    std::promise<void> safePromise;
    auto safeFuture = safePromise.get_future();

    std::atomic<bool> thrownSignaled{false};
    std::atomic<bool> safeSignaled{false};

    const std::string throwUri = "file:///trigger_throw_" + test::GenerateRandomSymbolName() + ".as";
    const std::string safeUri = "file:///safe_doc_" + test::GenerateRandomSymbolName() + ".as";

    AnalysisScheduler scheduler(
        [&](AnalyzeRequest req)
        {
            if (req.uriStr == throwUri)
            {
                if (!thrownSignaled.exchange(true))
                {
                    thrownPromise.set_value();
                }
                throw std::runtime_error("Simulated catastrophic analysis failure");
            }
            if (req.uriStr == safeUri)
            {
                if (!safeSignaled.exchange(true))
                {
                    safePromise.set_value();
                }
            }
        },
        std::chrono::milliseconds(0));

    scheduler.ScheduleImmediate(throwUri, "int a = 1;", true, document::MakeTreePtr(nullptr));
    thrownFuture.get();
    CHECK(thrownSignaled.load());

    scheduler.ScheduleImmediate(safeUri, "int b = 2;", true, document::MakeTreePtr(nullptr));
    safeFuture.get();
    CHECK(safeSignaled.load());

    scheduler.Stop();
}

TEST_CASE("ConcurrencySafety - Tree-Sitter Error Node and Null Tolerance Invariant")
{
    SUBCASE("Unterminated string literal")
    {
        AssertMalformedSnippetTolerance("string str = \"unterminated string literal;\nint next = 42;", 0, 16);
    }

    SUBCASE("Dangling member access dot")
    {
        AssertMalformedSnippetTolerance("void Main() { obj.; }", 0, 18);
    }

    SUBCASE("Severely corrupted syntax tokens")
    {
        AssertMalformedSnippetTolerance("class { int %%% $$$ @@@ void = ; }", 0, 14);
    }

    SUBCASE("Completely empty document")
    {
        AssertMalformedSnippetTolerance("", 0, 0);
    }
}
