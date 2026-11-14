#include "features/document_link/DocumentLinkHandler.h"
#include "parser/AngelScriptParser.h"
#include "utils/IncludeResolver.h"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace angel_lsp;

// =====================================================================================
// Like WorkspaceIncludeGraphTest, these need files that really exist: a directive only
// counts as resolved once IncludeResolver finds something on disk behind it.
// =====================================================================================

namespace
{
struct LinkFixture
{
    std::filesystem::path dir;
    std::vector<std::string> searchDirectories;

    LinkFixture()
    {
        const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        dir = std::filesystem::temp_directory_path() / ("angel_lsp_links_" + unique);
        std::filesystem::create_directories(dir);
        std::error_code ec;
        auto c = std::filesystem::canonical(dir, ec);
        if (!ec)
            dir = std::move(c);
    }

    ~LinkFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void Write(const std::string& name, const std::string& content) const
    {
        std::ofstream out(dir / name, std::ios::binary);
        out << content;
    }

    std::string Uri(const std::string& name) const
    {
        return lsp::Uri::fileUriFromPath(utils::IncludeResolver::NormalizePath(dir / name)).toString();
    }
};
} // namespace

TEST_SUITE("features/DocumentLink")
{
    TEST_CASE("GetDocumentLinks - a resolvable include becomes a link over the quoted path")
    {
        LinkFixture fixture;
        fixture.Write("base.as", "void Base() {}\n");

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "#include \"base.as\"\nvoid Main() {}\n";

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        const auto links = features::GetDocumentLinks(request);

        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);

        const lsp::DocumentLink& link = links->front();
        CHECK(link.range.start.line == 0);

        // The range covers "base.as" only, not the quotes or the directive.
        CHECK(source.substr(link.range.start.character, link.range.end.character - link.range.start.character) ==
              "base.as");
        REQUIRE(link.target.has_value());
        CHECK(std::string(link.target->toString()).ends_with("base.as"));
        CHECK_FALSE(link.tooltip.has_value());
    }

    TEST_CASE("GetDocumentLinks - an indented directive still gets the right columns")
    {
        LinkFixture fixture;
        fixture.Write("base.as", "void Base() {}\n");

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "    #include \"base.as\"\n";

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        const auto links = features::GetDocumentLinks(request);

        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);
        CHECK(source.substr(links->front().range.start.character,
                            links->front().range.end.character - links->front().range.start.character) == "base.as");
    }

    TEST_CASE("GetDocumentLinks - a directive on a later line reports that line")
    {
        LinkFixture fixture;
        fixture.Write("base.as", "void Base() {}\n");

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "// header\n\n#include \"base.as\"\n";

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        const auto links = features::GetDocumentLinks(request);

        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);
        CHECK(links->front().range.start.line == 2);
    }

    TEST_CASE("GetDocumentLinks - several includes produce one link each")
    {
        LinkFixture fixture;
        fixture.Write("a.as", "void A() {}\n");
        fixture.Write("b.as", "void B() {}\n");

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "#include \"a.as\"\n#include \"b.as\"\n";

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        const auto links = features::GetDocumentLinks(request);

        REQUIRE(links.has_value());
        CHECK(links->size() == 2);
    }

    TEST_CASE("GetDocumentLinks - an unresolvable include produces no link")
    {
        LinkFixture fixture;

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "#include \"missing.as\"\n";

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        CHECK_FALSE(features::GetDocumentLinks(request).has_value());
    }

    TEST_CASE("GetDocumentLinks - a document with no directives yields nothing")
    {
        LinkFixture fixture;

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "void Main() {}\n";
        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};

        CHECK_FALSE(features::GetDocumentLinks(request).has_value());
    }

    TEST_CASE("GetDocumentLinks - a directive inside a comment is not a link")
    {
        LinkFixture fixture;
        fixture.Write("base.as", "void Base() {}\n");

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "// #include \"base.as\"\n";
        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};

        CHECK_FALSE(features::GetDocumentLinks(request).has_value());
    }

    // -------------------------------------------------------------------------------------
    // Unresolved-include diagnostics
    // -------------------------------------------------------------------------------------

    TEST_CASE("GetUnresolvedIncludeDiagnostics - warns about an include that resolves to nothing")
    {
        LinkFixture fixture;

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "#include \"missing.as\"\n";

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        const auto diagnostics = features::GetUnresolvedIncludeDiagnostics(request);

        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "as-warn-include-not-found");
        CHECK(diagnostics[0].severity == analysis::DiagnosticSeverity::Warning);
        CHECK(diagnostics[0].range.start.line == 0);
        CHECK(diagnostics[0].message.find("missing.as") != std::string::npos);
        CHECK(diagnostics[0].fileUri == uri);
    }

    TEST_CASE("GetUnresolvedIncludeDiagnostics - says nothing about includes that do resolve")
    {
        LinkFixture fixture;
        fixture.Write("base.as", "void Base() {}\n");

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "#include \"base.as\"\n";
        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};

        CHECK(features::GetUnresolvedIncludeDiagnostics(request).empty());
    }

    TEST_CASE("GetUnresolvedIncludeDiagnostics - reports each broken directive separately")
    {
        LinkFixture fixture;
        fixture.Write("ok.as", "void Ok() {}\n");

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "#include \"gone.as\"\n#include \"ok.as\"\n#include \"also_gone.as\"\n";

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        const auto diagnostics = features::GetUnresolvedIncludeDiagnostics(request);

        REQUIRE(diagnostics.size() == 2);
        CHECK(diagnostics[0].range.start.line == 0);
        CHECK(diagnostics[1].range.start.line == 2);
    }

    TEST_CASE("GetUnresolvedIncludeDiagnostics - a search directory rescues an otherwise broken include")
    {
        LinkFixture fixture;
        std::filesystem::create_directories(fixture.dir / "shared");
        std::ofstream(fixture.dir / "shared" / "util.as", std::ios::binary) << "void Util() {}\n";

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "#include \"util.as\"\n";

        features::DocumentLinkRequest without{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        CHECK(features::GetUnresolvedIncludeDiagnostics(without).size() == 1);

        const std::vector<std::string> withShared = {utils::IncludeResolver::NormalizePath(fixture.dir / "shared")};
        features::DocumentLinkRequest with{uri, source, {.searchDirectories = withShared}, nullptr};
        CHECK(features::GetUnresolvedIncludeDiagnostics(with).empty());
    }

    TEST_CASE("DocumentLink - A missing include inside a dead #if is not reported")
    {
        // Measured through the oracle, and it is the whole reason this filter exists: a missing file
        // included from inside `#if UNDEFINED` compiles (exit 0), because CScriptBuilder blanks the
        // region in its first pass and only looks for `#include` in its second. The identical
        // directive one line further down is rejected (exit 1).
        const std::string source = "#if NEVER_DEFINED\n"
                                   "#include \"there_is_no_such_file.as\"\n"
                                   "#endif\n"
                                   "#include \"there_is_no_such_file.as\"\n"
                                   "void main() { }\n";

        const std::vector<std::string> searchDirectories;
        const std::vector<std::string> allowedRoots{"/workspace"};

        features::DocumentLinkRequest request{"file:///workspace/main.as",
                                              source,
                                              {.searchDirectories = searchDirectories, .allowedRoots = allowedRoots},
                                              nullptr};
        request.excludedLineRanges = angel_lsp::utils::FindExcludedLineRanges(source);

        const auto diagnostics = features::GetUnresolvedIncludeDiagnostics(request);

        // Exactly one, and it is the live one on line 3 - not the identical text on line 1.
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].range.start.line == 3);
        CHECK(diagnostics[0].code == "as-warn-include-not-found");
    }

    TEST_CASE("GetDocumentLinks - with NodeIndex resolves include directives directly from index")
    {
        LinkFixture fixture;
        fixture.Write("base.as", "void Base() {}\n");

        const std::string uri = fixture.Uri("main.as");
        const std::string source = "#include \"base.as\"\nvoid Main() {}\n";

        parser::AngelScriptParser parser;
        auto* tree = parser.Parse(source);
        REQUIRE(tree != nullptr);
        analysis::NodeIndex index(ts_tree_root_node(tree), analysis::NodeIndexOptions{.source = source});

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        request.nodeIndex = &index;
        const auto links = features::GetDocumentLinks(request);

        REQUIRE(links.has_value());
        REQUIRE(links->size() == 1);

        const lsp::DocumentLink& link = links->front();
        CHECK(link.range.start.line == 0);
        CHECK(source.substr(link.range.start.character, link.range.end.character - link.range.start.character) ==
              "base.as");
        REQUIRE(link.target.has_value());
        CHECK(std::string(link.target->toString()).ends_with("base.as"));

        ts_tree_delete(tree);
    }

    TEST_CASE("GetDocumentLinks - with NodeIndex and no includes returns nullopt without fallback scan")
    {
        LinkFixture fixture;
        const std::string uri = fixture.Uri("main.as");
        const std::string source = "void Main() { int x = 42; }\n";

        parser::AngelScriptParser parser;
        auto* tree = parser.Parse(source);
        REQUIRE(tree != nullptr);
        analysis::NodeIndex index(ts_tree_root_node(tree), analysis::NodeIndexOptions{.source = source});

        features::DocumentLinkRequest request{uri, source, {.searchDirectories = fixture.searchDirectories}, nullptr};
        request.nodeIndex = &index;
        const auto links = features::GetDocumentLinks(request);
        CHECK_FALSE(links.has_value());

        const auto diags = features::GetUnresolvedIncludeDiagnostics(request);
        CHECK(diags.empty());

        ts_tree_delete(tree);
    }

    TEST_CASE("GetUnresolvedIncludeDiagnostics - with NodeIndex reports missing include and obeys exclusion")
    {
        const std::string source = "#if 0\n"
                                   "#include \"missing_dead.as\"\n"
                                   "#endif\n"
                                   "#include \"missing_live.as\"\n"
                                   "void main() {}\n";

        parser::AngelScriptParser parser;
        auto* tree = parser.Parse(source);
        REQUIRE(tree != nullptr);
        analysis::NodeIndex index(ts_tree_root_node(tree), analysis::NodeIndexOptions{.source = source});

        const std::vector<std::string> searchDirectories;
        const std::vector<std::string> allowedRoots{"/workspace"};
        features::DocumentLinkRequest request{"file:///workspace/main.as",
                                              source,
                                              {.searchDirectories = searchDirectories, .allowedRoots = allowedRoots},
                                              nullptr};
        request.nodeIndex = &index;
        request.excludedLineRanges = angel_lsp::utils::FindExcludedLineRanges(source);

        const auto diags = features::GetUnresolvedIncludeDiagnostics(request);
        REQUIRE(diags.size() == 1);
        CHECK(diags[0].range.start.line == 3);
        CHECK(diags[0].code == "as-warn-include-not-found");

        ts_tree_delete(tree);
    }
}
