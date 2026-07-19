#include <doctest/doctest.h>
#include "analysis/DiagnosticCache.h"

using namespace analysis;

TEST_SUITE("DiagnosticCache")
{
    TEST_CASE("DC1 & DC2: Update and GetAt functionality")
    {
        DiagnosticCache cache;
        std::vector<lsp::Diagnostic> diags;

        lsp::Diagnostic d1;
        d1.message = "Syntax error";
        d1.range.start = {5, 0};
        d1.range.end = {5, 10};
        d1.severity = lsp::DiagnosticSeverity::Error;

        lsp::Diagnostic d2;
        d2.message = "Unused variable";
        d2.range.start = {10, 0};
        d2.range.end = {10, 5};
        d2.severity = lsp::DiagnosticSeverity::Warning;

        diags.push_back(d1);
        diags.push_back(d2);

        cache.Update("file:///test.as", std::move(diags));

        // DC1: Find existing diagnostic
        auto found = cache.GetAt("file:///test.as", 5, 2);
        REQUIRE(found.size() == 1);
        CHECK(found[0]->message == "Syntax error");
        CHECK(found[0]->severity == lsp::DiagnosticSeverity::Error); // DC3: Error severity

        auto foundWarn = cache.GetAt("file:///test.as", 10, 2);
        REQUIRE(foundWarn.size() == 1);
        CHECK(foundWarn[0]->message == "Unused variable");
        CHECK(foundWarn[0]->severity == lsp::DiagnosticSeverity::Warning); // DC3: Warning severity

        // DC2: Return empty for lines without diagnostics
        auto notFound = cache.GetAt("file:///test.as", 6, 0);
        CHECK(notFound.empty());

        // Return empty for different URI
        auto notFoundUri = cache.GetAt("file:///other.as", 5, 2);
        CHECK(notFoundUri.empty());
    }

    TEST_CASE("DC4: Clear removes only specific URI")
    {
        DiagnosticCache cache;

        std::vector<lsp::Diagnostic> diags1(1);
        diags1[0].message = "Error 1";
        diags1[0].range.start.line = 1;
        diags1[0].range.end.line = 1;

        std::vector<lsp::Diagnostic> diags2(1);
        diags2[0].message = "Error 2";
        diags2[0].range.start.line = 2;
        diags2[0].range.end.line = 2;

        cache.Update("file:///a.as", std::move(diags1));
        cache.Update("file:///b.as", std::move(diags2));

        CHECK(cache.GetAt("file:///a.as", 1, 0).size() == 1);
        CHECK(cache.GetAt("file:///b.as", 2, 0).size() == 1);

        cache.Clear("file:///a.as");

        CHECK(cache.GetAt("file:///a.as", 1, 0).empty());
        CHECK(cache.GetAt("file:///b.as", 2, 0).size() == 1); // Not affected
    }
}
