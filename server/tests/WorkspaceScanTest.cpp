#include <doctest/doctest.h>

#include "utils/WorkspaceScan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace angel_lsp::utils;

// =====================================================================================
// The one workspace walk.
//
// It replaced three copies whose own comments recorded the same two bugs being found and fixed
// separately in each: a permission-protected directory ending the walk, and an excluded tree being
// filtered file by file instead of pruned. Those two properties, and the cancellation contract,
// are what this file pins - the third copy had a check the other two never got, which is exactly
// the drift that gets expensive.
// =====================================================================================

namespace
{
    /** @brief A throwaway tree under the system temp directory, removed on destruction. */
    struct ScanFixture
    {
        std::filesystem::path root;

        ScanFixture()
        {
            root = std::filesystem::temp_directory_path() /
                   ("angel_scan_" + std::to_string(std::filesystem::hash_value(
                                        std::filesystem::temp_directory_path())) +
                    std::to_string(reinterpret_cast<uintptr_t>(this)));
            std::filesystem::create_directories(root);
        }

        ~ScanFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }

        void Write(const std::string &relative)
        {
            const std::filesystem::path target = root / relative;
            std::filesystem::create_directories(target.parent_path());
            std::ofstream out(target);
            out << "void main() { }\n";
        }

        std::vector<std::string> Roots() const { return { root.generic_string() }; }
    };

    std::vector<std::string> NamesOf(const std::vector<std::filesystem::path> &paths)
    {
        std::vector<std::string> names;
        names.reserve(paths.size());
        for (const auto &path : paths)
            names.push_back(path.filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }
}

TEST_CASE("WorkspaceScan - Every regular file under a root is visited")
{
    ScanFixture fixture;
    fixture.Write("a.as");
    fixture.Write("nested/b.as");
    fixture.Write("nested/deeper/c.as");

    std::vector<std::filesystem::path> seen;
    const bool completed = ForEachWorkspaceFile(
        fixture.Roots(), {}, {},
        [&seen](const std::filesystem::directory_entry &entry) { seen.push_back(entry.path()); });

    CHECK(completed);
    CHECK(NamesOf(seen) == std::vector<std::string>{ "a.as", "b.as", "c.as" });
}

TEST_CASE("WorkspaceScan - An excluded directory is pruned, not filtered")
{
    // The distinction is the point. A filter still descends into the tree and rejects every file
    // in it one at a time, which on a build directory is the difference between a fast scan and a
    // slow one. Observable here as the walk never reaching the file inside.
    ScanFixture fixture;
    fixture.Write("keep.as");
    fixture.Write("build/generated.as");
    fixture.Write("build/deeper/also_generated.as");

    std::vector<std::filesystem::path> seen;
    const bool completed = ForEachWorkspaceFile(
        fixture.Roots(), { "**/build/**" }, {},
        [&seen](const std::filesystem::directory_entry &entry) { seen.push_back(entry.path()); });

    CHECK(completed);
    CHECK(NamesOf(seen) == std::vector<std::string>{ "keep.as" });
}

TEST_CASE("WorkspaceScan - A root that does not exist is skipped, not thrown from")
{
    // Two of the three walks this replaced passed an error_code to the iterator and then ignored
    // it, leaving the following loop reading an iterator that had failed to open anything.
    ScanFixture fixture;
    fixture.Write("real.as");

    std::vector<std::string> roots = { (fixture.root / "no_such_directory").generic_string() };
    for (const auto &root : fixture.Roots())
        roots.push_back(root);

    std::vector<std::filesystem::path> seen;
    const bool completed = ForEachWorkspaceFile(
        roots, {}, {},
        [&seen](const std::filesystem::directory_entry &entry) { seen.push_back(entry.path()); });

    // The bad root cost nothing: the good one after it was still walked.
    CHECK(completed);
    CHECK(NamesOf(seen) == std::vector<std::string>{ "real.as" });
}

TEST_CASE("WorkspaceScan - A stop request ends the walk and is reported as not completed")
{
    // Callers act on the difference: one closes out its progress notification, one declines to
    // detect an engine profile from a partial file list, and one leaves the existing include graph
    // in place rather than publishing half of a new one.
    ScanFixture fixture;
    for (int i = 0; i < 20; ++i)
        fixture.Write("file" + std::to_string(i) + ".as");

    int visited = 0;
    const bool completed = ForEachWorkspaceFile(
        fixture.Roots(), {},
        [&visited]() { return visited >= 3; },
        [&visited](const std::filesystem::directory_entry &) { ++visited; });

    CHECK_FALSE(completed);

    // Stopped early rather than merely reported as stopped, which a walk that ran to the end and
    // returned false would also do.
    CHECK(visited == 3);
}

TEST_CASE("WorkspaceScan - A stop that is already true visits nothing")
{
    ScanFixture fixture;
    fixture.Write("a.as");

    int visited = 0;
    const bool completed = ForEachWorkspaceFile(
        fixture.Roots(), {},
        []() { return true; },
        [&visited](const std::filesystem::directory_entry &) { ++visited; });

    CHECK_FALSE(completed);
    CHECK(visited == 0);
}

TEST_CASE("WorkspaceScan - No roots is a completed walk over nothing")
{
    int visited = 0;
    const bool completed = ForEachWorkspaceFile(
        {}, {}, {}, [&visited](const std::filesystem::directory_entry &) { ++visited; });

    CHECK(completed);
    CHECK(visited == 0);
}
