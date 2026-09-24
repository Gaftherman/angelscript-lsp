#include <doctest/doctest.h>

#include "analysis/DocComment.h"
#include "analysis/DoxygenMarkdown.h"
#include "helpers/TestUtils.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;

namespace
{
/**
 * @brief Asserts that rendered Markdown output contains no orphaned semicolon paragraphs.
 * @param[in] rendered Output markdown string.
 */
void AssertNoOrphanedSemicolons(const std::string& rendered)
{
    CHECK(rendered.find("\n;\n") == std::string::npos);
    CHECK(rendered.find("\n\n;\n\n") == std::string::npos);
    CHECK(rendered.find("\n\n;") == std::string::npos);
    CHECK(rendered != ";");
}
} // namespace

TEST_CASE("DocCommentFuzzer - Sven Co-op exact comment pattern produces clean markdown without stray semicolons")
{
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("ClientPutInServerHook");
    const std::string source =
        "//Called when a player has finished connecting and is put into the world. "
        "It is safe to send network messages to the player at this point.;\n"
        "funcdef HookReturnCode " + fnName + "(CBasePlayer@);\n";

    const std::string doc = ExtractDocComment(source, 1);
    CHECK(!doc.empty());
    CHECK(doc.find("Called when a player has finished connecting and is put into the world.") != std::string::npos);
    CHECK(doc.find("It is safe to send network messages to the player at this point.") != std::string::npos);
    AssertNoOrphanedSemicolons(doc);
}

TEST_CASE("DocCommentFuzzer - High-throughput randomized delimiter mutation fuzzer")
{
    std::mt19937 rng(42);
    const std::vector<std::string> suffixes = {
        ";", ".;", "..;", ". ;", ").;", "].;", "\"..;", "'. ;", "\t;", "  ;",
    };
    const std::vector<std::string> openers = {
        "//", "///", "/**", "/*!"
    };

    for (size_t iteration = 0; iteration < 64; ++iteration)
    {
        const std::string word1 = angel_lsp::test::GenerateRandomSymbolName("Action");
        const std::string word2 = angel_lsp::test::GenerateRandomSymbolName("Target");
        const std::string suffix = suffixes[rng() % suffixes.size()];
        const std::string opener = openers[rng() % openers.size()];

        std::string rawComment;
        if (opener.starts_with("/*"))
        {
            rawComment = opener + "\n * " + word1 + " the " + word2 + suffix + "\n */";
        }
        else
        {
            rawComment = opener + " " + word1 + " the " + word2 + suffix;
        }

        const std::string rendered = RenderDoxygenMarkdown(rawComment);
        CHECK(!rendered.empty());
        AssertNoOrphanedSemicolons(rendered);
    }
}

TEST_CASE("DocCommentFuzzer - Multi-sentence stub comments with punctuation invariants")
{
    for (size_t i = 0; i < 16; ++i)
    {
        const std::string id1 = angel_lsp::test::GenerateRandomSymbolName("Setup");
        const std::string id2 = angel_lsp::test::GenerateRandomSymbolName("Execute");
        const std::string id3 = angel_lsp::test::GenerateRandomSymbolName("Teardown");

        const std::string raw =
            "//" + id1 + " initializes the system. " + id2 + " executes step 1 (default 1.0f). " +
            id3 + " finalizes the session.;";

        const std::string rendered = RenderDoxygenMarkdown(raw);
        CHECK(!rendered.empty());
        CHECK(rendered.find(id1) != std::string::npos);
        CHECK(rendered.find(id2) != std::string::npos);
        CHECK(rendered.find(id3) != std::string::npos);
        AssertNoOrphanedSemicolons(rendered);
    }
}

TEST_CASE("DocCommentFuzzer - Code blocks preserve internal semicolons intact")
{
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("counter");
    const std::string input =
        "/**\n"
        " * @brief Sample code demo.\n"
        " * @code\n"
        " * int " + varName + " = 100;\n"
        " * Foo(" + varName + ");\n"
        " * @endcode\n"
        " */";

    const std::string rendered = RenderDoxygenMarkdown(input);
    CHECK(rendered.find("int " + varName + " = 100;") != std::string::npos);
    CHECK(rendered.find("Foo(" + varName + ");") != std::string::npos);
    AssertNoOrphanedSemicolons(rendered);
}

TEST_CASE("DocCommentFuzzer - Behavior macro comments retain trailing semicolons")
{
    const std::string macroName = "asBEHAVE_" + angel_lsp::test::GenerateRandomSymbolName("CONSTRUCT");
    const std::string source = "ref(); // " + macroName + ";\n";

    const std::string doc = ExtractDocComment(source, 0);
    CHECK(doc == macroName + ";");
}
