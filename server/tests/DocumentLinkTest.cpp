#include <doctest/doctest.h>

#include "features/document_link/DocumentLinkHandler.h"
#include "utils/IncludeResolver.h"

#include <chrono>
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
        }

        ~LinkFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Write(const std::string &name, const std::string &content) const
        {
            std::ofstream out(dir / name, std::ios::binary);
            out << content;
        }

        std::string Uri(const std::string &name) const
        {
            return lsp::Uri::fileUriFromPath(utils::IncludeResolver::NormalizePath(dir / name)).toString();
        }
    };
}

TEST_CASE("GetDocumentLinks - a resolvable include becomes a link over the quoted path")
{
    LinkFixture fixture;
    fixture.Write("base.as", "void Base() {}\n");

    const std::string uri = fixture.Uri("main.as");
    const std::string source = "#include \"base.as\"\nvoid Main() {}\n";

    features::DocumentLinkRequest request{uri, source, fixture.searchDirectories, nullptr};
    const auto links = features::GetDocumentLinks(request);

    REQUIRE(links.has_value());
    REQUIRE(links->size() == 1);

    const lsp::DocumentLink &link = links->front();
    CHECK(link.range.start.line == 0);

    // The range covers "base.as" only, not the quotes or the directive.
    CHECK(source.substr(link.range.start.character, link.range.end.character - link.range.start.character) == "base.as");
    REQUIRE(link.target.has_value());
    CHECK(std::string(link.target->toString()).ends_with("base.as"));
}

TEST_CASE("GetDocumentLinks - an indented directive still gets the right columns")
{
    LinkFixture fixture;
    fixture.Write("base.as", "void Base() {}\n");

    const std::string uri = fixture.Uri("main.as");
    const std::string source = "    #include \"base.as\"\n";

    features::DocumentLinkRequest request{uri, source, fixture.searchDirectories, nullptr};
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

    features::DocumentLinkRequest request{uri, source, fixture.searchDirectories, nullptr};
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

    features::DocumentLinkRequest request{uri, source, fixture.searchDirectories, nullptr};
    const auto links = features::GetDocumentLinks(request);

    REQUIRE(links.has_value());
    CHECK(links->size() == 2);
}

TEST_CASE("GetDocumentLinks - an unresolvable include produces no link")
{
    LinkFixture fixture;

    const std::string uri = fixture.Uri("main.as");
    const std::string source = "#include \"missing.as\"\n";

    features::DocumentLinkRequest request{uri, source, fixture.searchDirectories, nullptr};
    CHECK_FALSE(features::GetDocumentLinks(request).has_value());
}

TEST_CASE("GetDocumentLinks - a document with no directives yields nothing")
{
    LinkFixture fixture;

    const std::string uri = fixture.Uri("main.as");
    features::DocumentLinkRequest request{uri, "void Main() {}\n", fixture.searchDirectories, nullptr};

    CHECK_FALSE(features::GetDocumentLinks(request).has_value());
}

TEST_CASE("GetDocumentLinks - a directive inside a comment is not a link")
{
    LinkFixture fixture;
    fixture.Write("base.as", "void Base() {}\n");

    const std::string uri = fixture.Uri("main.as");
    features::DocumentLinkRequest request{uri, "// #include \"base.as\"\n", fixture.searchDirectories, nullptr};

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

    features::DocumentLinkRequest request{uri, source, fixture.searchDirectories, nullptr};
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
    features::DocumentLinkRequest request{uri, "#include \"base.as\"\n", fixture.searchDirectories, nullptr};

    CHECK(features::GetUnresolvedIncludeDiagnostics(request).empty());
}

TEST_CASE("GetUnresolvedIncludeDiagnostics - reports each broken directive separately")
{
    LinkFixture fixture;
    fixture.Write("ok.as", "void Ok() {}\n");

    const std::string uri = fixture.Uri("main.as");
    const std::string source = "#include \"gone.as\"\n#include \"ok.as\"\n#include \"also_gone.as\"\n";

    features::DocumentLinkRequest request{uri, source, fixture.searchDirectories, nullptr};
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

    features::DocumentLinkRequest without{uri, source, fixture.searchDirectories, nullptr};
    CHECK(features::GetUnresolvedIncludeDiagnostics(without).size() == 1);

    const std::vector<std::string> withShared = {utils::IncludeResolver::NormalizePath(fixture.dir / "shared")};
    features::DocumentLinkRequest with{uri, source, withShared, nullptr};
    CHECK(features::GetUnresolvedIncludeDiagnostics(with).empty());
}
