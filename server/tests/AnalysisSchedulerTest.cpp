#include <doctest/doctest.h>
#include "lsp/AnalysisScheduler.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace angel_lsp;

TEST_CASE("AnalysisScheduler - Debouncing collapses rapid edits")
{
    std::atomic<int> analyzeCount{ 0 };
    std::string lastAnalyzedUri;
    std::string lastAnalyzedText;

    AnalysisScheduler scheduler([&](const std::string &uri, const std::string &text,
                                    document::TreePtr /*tree*/, int /*version*/)
    {
        lastAnalyzedUri = uri;
        lastAnalyzedText = text;
        ++analyzeCount;
    }, std::chrono::milliseconds(50));

    const std::string uri = "file:///test.as";
    for (int i = 0; i < 5; ++i)
    {
        scheduler.Schedule(uri, "int x = " + std::to_string(i) + ";", false, document::MakeTreePtr(nullptr), i);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait past debounce window
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(analyzeCount == 1);
    CHECK(lastAnalyzedUri == uri);
    CHECK(lastAnalyzedText == "int x = 4;");

    scheduler.Stop();
}

TEST_CASE("AnalysisScheduler - MarkSaved cancels pending work")
{
    std::atomic<int> analyzeCount{ 0 };

    AnalysisScheduler scheduler([&](const std::string &/*uri*/, const std::string &/*text*/,
                                    document::TreePtr /*tree*/, int /*version*/)
    {
        ++analyzeCount;
    }, std::chrono::milliseconds(80));

    const std::string uri = "file:///saved.as";
    scheduler.Schedule(uri, "void f() {}", false, document::MakeTreePtr(nullptr), 1);
    scheduler.MarkSaved(uri);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    CHECK(analyzeCount == 0);
    scheduler.Stop();
}

TEST_CASE("AnalysisScheduler - Peer debounce window")
{
    AnalysisScheduler scheduler(nullptr, std::chrono::milliseconds(50));

    const std::string uri = "file:///peer.as";
    CHECK_FALSE(scheduler.ShouldDebouncePeer(uri)); // first time, not debounced
    CHECK(scheduler.ShouldDebouncePeer(uri));       // immediately after, debounced

    scheduler.ClearPeerDebounce(uri);
    CHECK_FALSE(scheduler.ShouldDebouncePeer(uri)); // cleared, not debounced

    scheduler.Stop();
}
