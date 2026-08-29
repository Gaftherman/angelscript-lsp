#include <doctest/doctest.h>

#include "helpers/TestUtils.h"

#include <string>

// =====================================================================================
// Stack-overflow resistance.
//
// Tree-sitter imposes no limit on how deep a tree may be, and a dozen checkers walk that tree
// recursively per analysis. A document made of a few thousand nested parentheses or braces is
// tiny - well under any size limit worth having - but produced a tree deep enough to run the
// stack out in every one of those walks. A language server takes its input from whatever file
// the user opens, so this is reachable by opening a file, not by any privileged action.
//
// These cases assert only that analysis completes. What diagnostics come back past the depth cap
// is deliberately unspecified: the cap abandons the subtree, and every consumer of a truncated
// resolution already treats "cannot see enough" as "stay silent". A missed diagnostic on
// pathological input costs nothing; a crash costs the session.
// =====================================================================================

using namespace angel_lsp::test;

namespace
{
    std::string Repeat(const std::string &unit, size_t times)
    {
        std::string out;
        out.reserve(unit.size() * times);
        for (size_t i = 0; i < times; ++i)
            out += unit;
        return out;
    }
}

TEST_CASE("Deep nesting - Thousands of nested parentheses do not overflow the stack")
{
    // Depth well past k_maxAstDepth (512), in a document of only a few kilobytes.
    const size_t depth = 4000;
    const std::string script =
        "void Main() {\n    int x = " + Repeat("(", depth) + "1" + Repeat(")", depth) + ";\n}\n";

    auto doc = CreateTestDocument("file:///deep_parens.as", script);
    REQUIRE(static_cast<bool>(doc));

    // Reaching here at all is the assertion: before the depth cap this recursed until it crashed.
    auto diagnostics = doc->GetDiagnostics();
    CHECK(diagnostics.size() < 10000);
}

TEST_CASE("Deep nesting - Deeply nested blocks do not overflow the stack")
{
    const size_t depth = 3000;
    const std::string script =
        "void Main() {\n" + Repeat("{\n", depth) + "int y = 1;\n" + Repeat("}\n", depth) + "}\n";

    auto doc = CreateTestDocument("file:///deep_blocks.as", script);
    REQUIRE(static_cast<bool>(doc));

    auto diagnostics = doc->GetDiagnostics();
    CHECK(diagnostics.size() < 10000);
}

TEST_CASE("Deep nesting - A long binary expression chain does not overflow the stack")
{
    // Left-associative chaining nests the tree once per operator, so this is depth, not width.
    const size_t terms = 4000;
    const std::string script =
        "void Main() {\n    int z = 1" + Repeat(" + 1", terms) + ";\n}\n";

    auto doc = CreateTestDocument("file:///deep_binary.as", script);
    REQUIRE(static_cast<bool>(doc));

    auto diagnostics = doc->GetDiagnostics();
    CHECK(diagnostics.size() < 10000);
}
