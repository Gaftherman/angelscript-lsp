#include "lsp/DocumentStore.h"
#include "parser/AngelScriptParser.h"
#include <doctest/doctest.h>
#include <thread>
#include <vector>

using namespace angel_lsp;

TEST_CASE("DocumentStore - Open, Update, Lookup, and Close operations")
{
    DocumentStore store;
    parser::AngelScriptParser parser;

    const std::string uri = "file:///test/main.as";
    const std::string text1 = "void main() {}";
    document::TreePtr tree1 = document::MakeTreePtr(parser.Parse(text1));

    CHECK_FALSE(store.IsOpen(uri));
    CHECK(store.GetText(uri) == std::nullopt);
    CHECK(store.GetVersion(uri) == -1);
    CHECK(store.GetTree(uri) == nullptr);

    // Open
    store.OpenDocument(DocumentStore::OpenDocumentRequest{uri, text1, 1, std::move(tree1), "file:///test/main.as"});
    CHECK(store.IsOpen(uri));
    CHECK(store.Size() == 1);
    CHECK(store.GetVersion(uri) == 1);
    CHECK(store.GetClientUri(uri) == "file:///test/main.as");
    auto retrievedText = store.GetText(uri);
    REQUIRE(retrievedText.has_value());
    CHECK(*retrievedText == text1);
    CHECK(store.GetTree(uri) != nullptr);

    // Update
    const std::string text2 = "void main() { int x = 1; }";
    document::TreePtr tree2 = document::MakeTreePtr(parser.Parse(text2));
    store.UpdateDocument(uri, text2, 2, std::move(tree2));
    CHECK(store.GetVersion(uri) == 2);
    retrievedText = store.GetText(uri);
    REQUIRE(retrievedText.has_value());
    CHECK(*retrievedText == text2);
    CHECK(store.GetTree(uri) != nullptr);

    // Snapshot & OpenUris
    auto snapshot = store.GetSnapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].first == uri);
    CHECK(snapshot[0].second == text2);

    auto openUris = store.GetOpenUris();
    REQUIRE(openUris.size() == 1);
    CHECK(openUris[0] == uri);

    // Close
    store.CloseDocument(uri);
    CHECK_FALSE(store.IsOpen(uri));
    CHECK(store.Size() == 0);
    CHECK(store.GetText(uri) == std::nullopt);
    CHECK(store.GetTree(uri) == nullptr);
}

TEST_CASE("DocumentStore - Concurrent read/write stress")
{
    DocumentStore store;
    constexpr int kThreads = 8;
    constexpr int kIterations = 100;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(
            [&store, t]()
            {
                const std::string uri = "file:///worker_" + std::to_string(t) + ".as";
                for (int i = 0; i < kIterations; ++i)
                {
                    store.OpenDocument(uri, "int x = " + std::to_string(i) + ";", i, document::MakeTreePtr(nullptr));
                    store.SetVersion(uri, i + 1);
                    auto text = store.GetText(uri);
                    CHECK(text.has_value());
                    CHECK(store.GetVersion(uri) >= i);
                    auto snapshot = store.GetSnapshot();
                    CHECK_FALSE(snapshot.empty());
                    store.CloseDocument(uri);
                }
            });
    }

    for (auto& w : workers)
    {
        w.join();
    }

    CHECK(store.Size() == 0);
}

TEST_CASE("DocumentStore - Generation invalidation on close and reopen")
{
    DocumentStore store;
    const std::string uri = "file:///gen_test.as";

    store.OpenDocument(uri, "void main() {}", 1, document::MakeTreePtr(nullptr));
    const uint64_t gen1 = store.GetGeneration(uri);
    CHECK(gen1 > 0);
    CHECK(store.IsCurrent(uri, gen1, 1));
    CHECK(store.IsCurrent(uri, gen1, 2));
    CHECK_FALSE(store.IsCurrent(uri, gen1, 0)); // Version 0 is older than version 1

    // Close document
    store.CloseDocument(uri);
    CHECK_FALSE(store.IsCurrent(uri, gen1, 1));
    CHECK(store.GetGeneration(uri) == 0);

    // Reopen document -> new generation
    store.OpenDocument(uri, "void main() { int y = 2; }", 1, document::MakeTreePtr(nullptr));
    const uint64_t gen2 = store.GetGeneration(uri);
    CHECK(gen2 > gen1);
    CHECK_FALSE(store.IsCurrent(uri, gen1, 1)); // Stale gen1 must be rejected
    CHECK(store.IsCurrent(uri, gen2, 1));       // Fresh gen2 is accepted
}
