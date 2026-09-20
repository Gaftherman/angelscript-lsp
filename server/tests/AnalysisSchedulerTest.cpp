#include "lsp/AnalysisScheduler.h"
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace angel_lsp;

TEST_CASE("AnalysisScheduler - Debouncing collapses rapid edits")
{
    std::atomic<int> analyzeCount{0};
    std::mutex stateMutex;
    std::string lastAnalyzedUri;
    std::string lastAnalyzedText;

    AnalysisScheduler scheduler(
        [&](const std::string& uri, const std::string& text, document::TreePtr /*tree*/, int /*version*/,
            uint64_t /*generation*/, uint64_t /*configRevision*/)
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            lastAnalyzedUri = uri;
            lastAnalyzedText = text;
            ++analyzeCount;
        },
        std::chrono::milliseconds(100));

    const std::string uri = "file:///test.as";
    for (int i = 0; i < 5; ++i)
    {
        scheduler.Schedule(uri, "int x = " + std::to_string(i) + ";", false, document::MakeTreePtr(nullptr), i);
    }

    // Deterministic barrier blocks until debounce and background analysis complete
    scheduler.DrainQueue();

    CHECK(analyzeCount.load() == 1);
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        CHECK(lastAnalyzedUri == uri);
        CHECK(lastAnalyzedText == "int x = 4;");
    }

    scheduler.Stop();
}

TEST_CASE("AnalysisScheduler - MarkSaved cancels pending work")
{
    std::atomic<int> analyzeCount{0};

    AnalysisScheduler scheduler([&](const std::string& /*uri*/, const std::string& /*text*/, document::TreePtr /*tree*/,
                                    int /*version*/, uint64_t /*generation*/, uint64_t /*configRevision*/)
                                { ++analyzeCount; },
                                std::chrono::milliseconds(100));

    const std::string uri = "file:///saved.as";
    scheduler.Schedule(uri, "void f() {}", false, document::MakeTreePtr(nullptr), 1);
    scheduler.MarkSaved(uri, 1);

    scheduler.DrainQueue();

    CHECK(analyzeCount.load() == 0);
    scheduler.Stop();
}

TEST_CASE("AnalysisScheduler - Peer debounce window")
{
    AnalysisScheduler scheduler(nullptr, std::chrono::milliseconds(50));

    const std::string uri = "file:///peer.as";
    CHECK_FALSE(scheduler.ShouldDebouncePeer(uri));
    CHECK(scheduler.ShouldDebouncePeer(uri));

    scheduler.ClearPeerDebounce(uri);
    CHECK_FALSE(scheduler.ShouldDebouncePeer(uri));

    scheduler.Stop();
}

TEST_CASE("AnalysisScheduler - Version-gated cancellation discards stale pending versions")
{
    std::atomic<int> analyzeCount{0};
    std::atomic<int> analyzedVersion{-1};
    std::mutex stateMutex;
    std::string analyzedText;

    AnalysisScheduler scheduler(
        [&](const std::string& /*uri*/, const std::string& text, document::TreePtr /*tree*/, int version,
            uint64_t /*generation*/, uint64_t /*configRevision*/)
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            analyzedVersion.store(version);
            analyzedText = text;
            ++analyzeCount;
        },
        std::chrono::milliseconds(100));

    const std::string uri = "file:///version_test.as";

    scheduler.Schedule(uri, "version 10", false, document::MakeTreePtr(nullptr), 10);
    scheduler.Schedule(uri, "version 5", false, document::MakeTreePtr(nullptr), 5);

    scheduler.DrainQueue();

    CHECK(analyzeCount.load() == 1);
    CHECK(analyzedVersion.load() == 10);
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        CHECK(analyzedText == "version 10");
    }

    scheduler.Stop();
}

TEST_CASE("AnalysisScheduler - Identical text with higher version updates pending version")
{
    std::atomic<int> analyzeCount{0};
    std::atomic<int> analyzedVersion{-1};

    AnalysisScheduler scheduler(
        [&](const std::string& /*uri*/, const std::string& /*text*/, document::TreePtr /*tree*/, int version,
            uint64_t /*generation*/, uint64_t /*configRevision*/)
        {
            analyzedVersion.store(version);
            ++analyzeCount;
        },
        std::chrono::milliseconds(100));

    const std::string uri = "file:///same_text.as";
    scheduler.Schedule(uri, "const int x = 42;", false, document::MakeTreePtr(nullptr), 1);
    scheduler.Schedule(uri, "const int x = 42;", false, document::MakeTreePtr(nullptr), 2);

    scheduler.DrainQueue();

    CHECK(analyzeCount.load() == 1);
    CHECK(analyzedVersion.load() == 2);

    scheduler.Stop();
}

TEST_CASE("AnalysisScheduler - Cancel removes pending and cancels active analysis")
{
    std::atomic<int> analyzeCount{0};

    AnalysisScheduler scheduler([&](const std::string& /*uri*/, const std::string& /*text*/, document::TreePtr /*tree*/,
                                    int /*version*/, uint64_t /*generation*/, uint64_t /*configRevision*/)
                                { ++analyzeCount; },
                                std::chrono::milliseconds(100));

    const std::string uri = "file:///cancelled.as";
    scheduler.Schedule(uri, "int a = 1;", false, document::MakeTreePtr(nullptr), 1);
    scheduler.Cancel(uri);

    scheduler.DrainQueue();

    CHECK(analyzeCount.load() == 0);
    CHECK(scheduler.IsCancelled(uri));

    scheduler.Stop();
}

TEST_CASE("AnalysisScheduler - DrainQueue provides deterministic barrier across multiple tasks")
{
    std::atomic<int> analyzeCount{0};
    AnalysisScheduler scheduler([&](const std::string& /*uri*/, const std::string& /*text*/, document::TreePtr /*tree*/,
                                    int /*version*/, uint64_t /*generation*/, uint64_t /*configRevision*/)
                                { ++analyzeCount; },
                                std::chrono::milliseconds(50));

    for (int i = 0; i < 5; ++i)
    {
        scheduler.Schedule("file:///task_" + std::to_string(i) + ".as", "content", true, document::MakeTreePtr(nullptr),
                           i);
    }

    scheduler.DrainQueue();

    CHECK(analyzeCount.load() == 5);
    scheduler.Stop();
}
