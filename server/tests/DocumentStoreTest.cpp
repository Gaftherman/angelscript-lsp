#include <doctest/doctest.h>
#include "lsp/DocumentStore.h"
#include "parser/AngelScriptParser.h"
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
    store.OpenDocument(uri, text1, 1, std::move(tree1), "file:///test/main.as");
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
        workers.emplace_back([&store, t]()
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

    for (auto &w : workers)
    {
        w.join();
    }

    CHECK(store.Size() == 0);
}
