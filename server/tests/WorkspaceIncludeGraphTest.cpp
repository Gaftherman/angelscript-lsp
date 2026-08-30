#include <doctest/doctest.h>

#include "utils/WorkspaceIncludeGraph.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace angel_lsp::utils;

// =====================================================================================
// These tests hit a real temporary directory rather than an injected file reader,
// because IncludeResolver::ResolveIncludePath decides whether a directive resolves by
// asking the filesystem whether the target exists. An in-memory fixture would resolve
// nothing and every graph would come back empty.
// =====================================================================================

namespace
{
    struct GraphFixture
    {
        std::filesystem::path dir;

        GraphFixture()
        {
            const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            dir = std::filesystem::temp_directory_path() / ("angel_lsp_graph_" + unique);
            std::filesystem::create_directories(dir);
            std::error_code ec;
            auto c = std::filesystem::canonical(dir, ec);
            if (!ec)
                dir = std::move(c);
        }

        ~GraphFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Write(const std::string &name, const std::string &content) const
        {
            const std::filesystem::path full = dir / name;
            if (full.has_parent_path())
                std::filesystem::create_directories(full.parent_path());

            std::ofstream out(full, std::ios::binary);
            out << content;
        }

        std::string Path(const std::string &name) const
        {
            return IncludeResolver::NormalizePath(dir / name);
        }

        std::string Root() const
        {
            return IncludeResolver::NormalizePath(dir);
        }

        // Held by the fixture rather than returned: a shared_mutex member makes the graph
        // neither copyable nor movable.
        WorkspaceIncludeGraph graph;

        void Build()
        {
            graph.Build({Root()}, {}, ".as");
        }

        std::set<std::string> ClosureOf(const std::string &name) const
        {
            const auto closure = graph.GetModuleClosure(Path(name));
            return std::set<std::string>(closure.begin(), closure.end());
        }
    };
}

// -------------------------------------------------------------------------------------
// The case that motivated the graph: a file is opened from the middle of a module and
// still needs the declarations that live in the file including it.
// -------------------------------------------------------------------------------------

TEST_CASE("GetModuleClosure - opening a leaf pulls in the files that include it, not just the ones it includes")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"mid.as\"\nvoid RootOnly() {}\n");
    fixture.Write("mid.as", "#include \"leaf.as\"\nvoid MidOnly() {}\n");
    fixture.Write("leaf.as", "void LeafOnly() { RootOnly(); }\n");

    fixture.Build();

    // Opening the leaf: nothing it includes, everything that includes it.
    const std::set<std::string> expected = {fixture.Path("root.as"), fixture.Path("mid.as"), fixture.Path("leaf.as")};
    CHECK(fixture.ClosureOf("leaf.as") == expected);

    // Opening any other member of the module yields the same set.
    CHECK(fixture.ClosureOf("root.as") == expected);
    CHECK(fixture.ClosureOf("mid.as") == expected);
}

TEST_CASE("GetModuleClosure - a sibling included by the same root comes along")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"a.as\"\n#include \"b.as\"\n");
    fixture.Write("a.as", "void A() {}\n");
    fixture.Write("b.as", "void B() {}\n");

    fixture.Build();

    const std::set<std::string> expected = {fixture.Path("root.as"), fixture.Path("a.as"), fixture.Path("b.as")};
    CHECK(fixture.ClosureOf("a.as") == expected);
}

TEST_CASE("GetModuleClosure - a diamond visits the shared file once")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"left.as\"\n#include \"right.as\"\n");
    fixture.Write("left.as", "#include \"common.as\"\n");
    fixture.Write("right.as", "#include \"common.as\"\n");
    fixture.Write("common.as", "void Common() {}\n");

    fixture.Build();

    const auto closure = fixture.graph.GetModuleClosure(fixture.Path("common.as"));
    CHECK(closure.size() == 4);
}

// -------------------------------------------------------------------------------------
// Isolation and independence
// -------------------------------------------------------------------------------------

TEST_CASE("GetModuleClosure - a file nothing includes and that includes nothing is its own module")
{
    GraphFixture fixture;
    fixture.Write("alone.as", "void Alone() {}\n");

    fixture.Build();

    const std::set<std::string> expected = {fixture.Path("alone.as")};
    CHECK(fixture.ClosureOf("alone.as") == expected);
}

TEST_CASE("GetModuleClosure - two independent modules do not bleed into each other")
{
    GraphFixture fixture;
    fixture.Write("weapons.as", "#include \"ak47.as\"\n");
    fixture.Write("ak47.as", "void Ak() {}\n");
    fixture.Write("menu.as", "#include \"buy.as\"\n");
    fixture.Write("buy.as", "void Buy() {}\n");

    fixture.Build();

    const std::set<std::string> weapons = {fixture.Path("weapons.as"), fixture.Path("ak47.as")};
    CHECK(fixture.ClosureOf("ak47.as") == weapons);

    const std::set<std::string> menu = {fixture.Path("menu.as"), fixture.Path("buy.as")};
    CHECK(fixture.ClosureOf("buy.as") == menu);
}

TEST_CASE("GetModuleClosure - a file the graph has never seen still resolves to itself")
{
    GraphFixture fixture;
    fixture.Write("known.as", "void Known() {}\n");

    fixture.Build();

    const auto closure = fixture.graph.GetModuleClosure(fixture.Path("never_scanned.as"));
    REQUIRE(closure.size() == 1);
    CHECK(closure[0] == fixture.Path("never_scanned.as"));
}

// -------------------------------------------------------------------------------------
// Degenerate graphs
// -------------------------------------------------------------------------------------

TEST_CASE("GetModuleClosure - a two-file cycle terminates and keeps both files together")
{
    GraphFixture fixture;
    fixture.Write("a.as", "#include \"b.as\"\n");
    fixture.Write("b.as", "#include \"a.as\"\n");

    fixture.Build();

    const std::set<std::string> expected = {fixture.Path("a.as"), fixture.Path("b.as")};
    CHECK(fixture.ClosureOf("a.as") == expected);
    CHECK(fixture.ClosureOf("b.as") == expected);
}

TEST_CASE("GetModuleClosure - a cycle hanging below a root still reaches the root")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"a.as\"\n");
    fixture.Write("a.as", "#include \"b.as\"\n");
    fixture.Write("b.as", "#include \"a.as\"\n");

    fixture.Build();

    const std::set<std::string> expected = {fixture.Path("root.as"), fixture.Path("a.as"), fixture.Path("b.as")};
    CHECK(fixture.ClosureOf("b.as") == expected);
}

TEST_CASE("Build - a directive pointing at a file that does not exist creates no edge")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"missing.as\"\n");

    fixture.Build();

    const std::set<std::string> expected = {fixture.Path("root.as")};
    CHECK(fixture.ClosureOf("root.as") == expected);
}

TEST_CASE("Build - directives inside comments and strings are not edges")
{
    GraphFixture fixture;
    fixture.Write("root.as", "// #include \"real.as\"\nstring s = \"#include \\\"real.as\\\"\";\n");
    fixture.Write("real.as", "void Real() {}\n");

    fixture.Build();

    const std::set<std::string> expected = {fixture.Path("root.as")};
    CHECK(fixture.ClosureOf("root.as") == expected);
}

TEST_CASE("Build - only files carrying the configured script extension are scanned")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"data.txt\"\n");
    fixture.Write("data.txt", "not a script\n");
    fixture.Write("notes.md", "#include \"root.as\"\n");

    fixture.Build();

    // notes.md was skipped, so it never became an includer of root.as.
    const std::set<std::string> expected = {fixture.Path("root.as"), fixture.Path("data.txt")};
    CHECK(fixture.ClosureOf("root.as") == expected);
}

// -------------------------------------------------------------------------------------
// UpdateFile - the save path
// -------------------------------------------------------------------------------------

TEST_CASE("UpdateFile - adding an include grows the module")
{
    GraphFixture fixture;
    fixture.Write("root.as", "void Root() {}\n");
    fixture.Write("extra.as", "void Extra() {}\n");

    fixture.Build();
    CHECK(fixture.ClosureOf("extra.as").size() == 1);

    fixture.graph.UpdateFile(fixture.Path("root.as"), "#include \"extra.as\"\n", {});

    const std::set<std::string> expected = {fixture.Path("root.as"), fixture.Path("extra.as")};
    CHECK(fixture.ClosureOf("extra.as") == expected);
}

TEST_CASE("UpdateFile - removing an include detaches the file instead of leaving a stale edge")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"extra.as\"\n");
    fixture.Write("extra.as", "void Extra() {}\n");

    fixture.Build();
    CHECK(fixture.ClosureOf("extra.as").size() == 2);

    fixture.graph.UpdateFile(fixture.Path("root.as"), "void Root() {}\n", {});

    const std::set<std::string> expected = {fixture.Path("extra.as")};
    CHECK(fixture.ClosureOf("extra.as") == expected);
}

TEST_CASE("Clear - drops every node and edge")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"leaf.as\"\n");
    fixture.Write("leaf.as", "void Leaf() {}\n");

    fixture.Build();
    CHECK(fixture.graph.FileCount() == 2);

    fixture.graph.Clear();

    CHECK(fixture.graph.FileCount() == 0);
    CHECK_FALSE(fixture.graph.Contains(fixture.Path("root.as")));
}

TEST_CASE("RemoveFile - drops the node and detaches it from both edge directions")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"middle.as\"\n");
    fixture.Write("middle.as", "#include \"leaf.as\"\n");
    fixture.Write("leaf.as", "void Leaf() {}\n");

    fixture.Build();
    REQUIRE(fixture.graph.FileCount() == 3);
    REQUIRE(fixture.ClosureOf("leaf.as").size() == 3);

    CHECK(fixture.graph.RemoveFile(fixture.Path("middle.as")));
    CHECK_FALSE(fixture.graph.Contains(fixture.Path("middle.as")));

    // The deleted file no longer holds leaf.as and root.as in one module: its forward edge to
    // leaf.as is gone, so leaf.as is now a module of its own.
    const std::set<std::string> expected = {fixture.Path("leaf.as")};
    CHECK(fixture.ClosureOf("leaf.as") == expected);
}

TEST_CASE("RemoveFile - reports whether the file was known")
{
    GraphFixture fixture;
    fixture.Write("root.as", "void Root() {}\n");
    fixture.Build();

    CHECK(fixture.graph.RemoveFile(fixture.Path("root.as")));
    CHECK_FALSE(fixture.graph.RemoveFile(fixture.Path("root.as")));
    CHECK_FALSE(fixture.graph.RemoveFile(fixture.Path("never-existed.as")));
}

TEST_CASE("RemoveFile - leaves the dangling directive of whoever still includes it")
{
    GraphFixture fixture;
    fixture.Write("root.as", "#include \"leaf.as\"\n");
    fixture.Write("leaf.as", "void Leaf() {}\n");

    fixture.Build();
    REQUIRE(fixture.graph.FileCount() == 2);

    fixture.graph.RemoveFile(fixture.Path("leaf.as"));

    // root.as still names leaf.as, because it does: rewriting its edges here would hide an
    // unresolved #include that the user needs to see reported.
    CHECK(fixture.graph.Contains(fixture.Path("root.as")));
    CHECK(fixture.ClosureOf("root.as").contains(fixture.Path("leaf.as")));
}
