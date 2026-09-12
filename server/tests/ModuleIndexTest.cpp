#include <doctest/doctest.h>
#include "lsp/ModuleIndex.h"
#include <thread>
#include <vector>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace angel_lsp;

namespace
{
    struct TempDirFixture
    {
        std::filesystem::path dir;

        TempDirFixture()
        {
            const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            dir = std::filesystem::temp_directory_path() / ("angel_lsp_modindex_" + unique);
            std::filesystem::create_directories(dir);
            std::error_code ec;
            auto c = std::filesystem::canonical(dir, ec);
            if (!ec)
            {
                dir = std::move(c);
            }
        }

        ~TempDirFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Write(const std::string &name, const std::string &content) const
        {
            const std::filesystem::path full = dir / name;
            if (full.has_parent_path())
            {
                std::filesystem::create_directories(full.parent_path());
            }

            std::ofstream out(full, std::ios::binary);
            out << content;
        }

        std::string Path(const std::string &name) const
        {
            return angel_lsp::utils::IncludeResolver::NormalizePath(dir / name);
        }

        std::string Root() const
        {
            return angel_lsp::utils::IncludeResolver::NormalizePath(dir);
        }
    };
}

TEST_CASE("ModuleIndex - Include graph and closure tracking")
{
    TempDirFixture fix;
    ModuleIndex index;

    const std::string contentA = "#include \"B.as\"\nvoid funcA() {}";
    const std::string contentB = "void funcB() {}";
    fix.Write("A.as", contentA);
    fix.Write("B.as", contentB);

    const std::string fileA = fix.Path("A.as");
    const std::string fileB = fix.Path("B.as");

    index.UpdateFile(fileA, contentA, {fix.Root()}, {fix.Root()}, "");
    index.UpdateFile(fileB, contentB, {fix.Root()}, {fix.Root()}, "");

    auto closureA = index.GetModuleClosure(fileA);
    CHECK(closureA.size() == 2);

    index.RemoveFile(fileA);
    auto closureAfterRemove = index.GetModuleClosure(fileA);
    CHECK(closureAfterRemove.size() == 1);
}

TEST_CASE("ModuleIndex - Closure lifecycle and refcounting")
{
    ModuleIndex index;

    const std::string doc1Uri = "file:///C:/project/doc1.as";
    const std::string doc2Uri = "file:///C:/project/doc2.as";
    const std::string sharedClosureUri = "file:///C:/project/shared.as";
    const std::string uniqueClosureUri = "file:///C:/project/unique.as";

    index.SetClosureDocument(sharedClosureUri, "void shared() {}");
    index.SetClosureDocument(uniqueClosureUri, "void unique() {}");

    CHECK(index.HasClosureDocument(sharedClosureUri));
    CHECK(index.HasClosureDocument(uniqueClosureUri));

    index.AssociateClosure(doc1Uri, {sharedClosureUri, uniqueClosureUri});
    index.AssociateClosure(doc2Uri, {sharedClosureUri});

    // Releasing doc1 should purge uniqueClosureUri because doc2 doesn't reference it,
    // but sharedClosureUri must remain because doc2 still references it.
    auto unreferenced1 = index.ReleaseClosure(doc1Uri);
    CHECK(unreferenced1.size() == 1);
    CHECK(unreferenced1[0] == uniqueClosureUri);
    CHECK_FALSE(index.HasClosureDocument(uniqueClosureUri));
    CHECK(index.HasClosureDocument(sharedClosureUri));

    // Releasing doc2 should purge sharedClosureUri
    auto unreferenced2 = index.ReleaseClosure(doc2Uri);
    CHECK(unreferenced2.size() == 1);
    CHECK(unreferenced2[0] == sharedClosureUri);
    CHECK_FALSE(index.HasClosureDocument(sharedClosureUri));
}

TEST_CASE("ModuleIndex - Indexed URI mappings and purge")
{
    ModuleIndex index;

    const std::string path = "C:/project/math.as";
    const std::string uri = "file:///C:/project/math.as";

    CHECK(index.GetIndexedUri(path) == std::nullopt);

    index.SetIndexedUri(path, uri);
    CHECK(index.GetIndexedUri(path) == uri);

    index.RemoveIndexedPath(path);
    CHECK(index.GetIndexedUri(path) == std::nullopt);

    index.SetIndexedUri(path, uri);
    index.SetClosureDocument(uri, "int add(int a, int b) { return a + b; }");
    CHECK(index.HasClosureDocument(uri));

    index.Clear();
    CHECK(index.GetIndexedUri(path) == std::nullopt);
    CHECK_FALSE(index.HasClosureDocument(uri));
}

TEST_CASE("ModuleIndex - Concurrent access")
{
    ModuleIndex index;
    constexpr int kIterations = 100;

    std::vector<std::thread> threads;
    threads.reserve(4);

    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&index, t]()
        {
            for (int i = 0; i < kIterations; ++i)
            {
                std::string path = "C:/project/file_" + std::to_string(t) + "_" + std::to_string(i) + ".as";
                std::string uri = "file:///" + path;
                index.SetIndexedUri(path, uri);
                index.SetClosureDocument(uri, "void f() {}");
                auto res = index.GetIndexedUri(path);
                CHECK(res.has_value());
                index.PurgeClosureDocument(uri);
            }
        });
    }

    for (auto &th : threads)
    {
        th.join();
    }
}
