#include <iostream>
#include <string>
#include <doctest/doctest.h>

#include "features/folding_range/FoldingRangeHandler.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::parser;

namespace
{
    struct FoldingTestEnv
    {
        AngelScriptParser parser;
        std::string uri = "file:///test.as";
        std::string source;
        TSTree *tree = nullptr;

        FoldingTestEnv(const std::string &code)
            : source(code)
        {
            tree = parser.Parse(source);
        }

        ~FoldingTestEnv()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<FoldingRangeResult> GetRanges()
        {
            FoldingRangeRequest req{
                uri,
                source,
                tree
            };
            return GetFoldingRanges(req);
        }
    };
}

TEST_CASE("FoldingRanges - Classes, Interfaces, and Namespaces")
{
    std::string code =
        "namespace Game\n"
        "{\n"
        "    class Player\n"
        "    {\n"
        "        int score;\n"
        "        void Reset()\n"
        "        {\n"
        "            score = 0;\n"
        "        }\n"
        "    }\n"
        "    interface IEntity\n"
        "    {\n"
        "        void Update();\n"
        "    }\n"
        "}\n";

    FoldingTestEnv env(code);
    auto ranges = env.GetRanges();
    REQUIRE(ranges.has_value());
    REQUIRE(ranges->size() >= 4);

    // Verify namespace Game fold (line 0 to 14 or 1 to 14)
    bool hasNamespace = false;
    bool hasClass = false;
    bool hasMethod = false;
    bool hasInterface = false;

    for (const auto &r : *ranges)
    {
        if (r.startLine <= 1 && r.endLine == 14)
        {
            hasNamespace = true;
        }
        if (r.startLine >= 2 && r.startLine <= 3 && r.endLine == 9)
        {
            hasClass = true;
        }
        if (r.startLine >= 5 && r.startLine <= 6 && r.endLine == 8)
        {
            hasMethod = true;
        }
        if (r.startLine >= 10 && r.startLine <= 11 && r.endLine == 13)
        {
            hasInterface = true;
        }
    }

    CHECK(hasNamespace);
    CHECK(hasClass);
    CHECK(hasMethod);
    CHECK(hasInterface);
}

TEST_CASE("FoldingRanges - Control Flow Statements")
{
    std::string code =
        "void TestControlFlow()\n"
        "{\n"
        "    if (true)\n"
        "    {\n"
        "        for (int i = 0; i < 10; ++i)\n"
        "        {\n"
        "            while (false)\n"
        "            {\n"
        "                int x = 1;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    switch (1)\n"
        "    {\n"
        "        case 1:\n"
        "            break;\n"
        "    }\n"
        "    try\n"
        "    {\n"
        "        int y = 2;\n"
        "    }\n"
        "    catch\n"
        "    {\n"
        "        int z = 3;\n"
        "    }\n"
        "}\n";

    FoldingTestEnv env(code);
    auto ranges = env.GetRanges();
    REQUIRE(ranges.has_value());
    CHECK(ranges->size() >= 5);

    // Ensure all ranges have startLine < endLine
    for (const auto &r : *ranges)
    {
        CHECK(r.startLine < r.endLine);
    }
}

TEST_CASE("FoldingRanges - Block Comments and Consecutive Single-Line Comments")
{
    std::string code =
        "/*\n"
        " * Multiline block comment\n"
        " * with multiple lines\n"
        " */\n"
        "void main() {\n"
        "    // Contiguous comment line 1\n"
        "    // Contiguous comment line 2\n"
        "    // Contiguous comment line 3\n"
        "    int a = 1;\n"
        "    // Single line comment (should NOT fold)\n"
        "    int b = 2;\n"
        "}\n";

    FoldingTestEnv env(code);
    auto ranges = env.GetRanges();
    REQUIRE(ranges.has_value());

    bool foundBlockComment = false;
    bool foundContiguousComment = false;
    bool foundSingleLineCommentFold = false;

    for (const auto &r : *ranges)
    {
        if (r.kind == lsp::FoldingRangeKind::Comment)
        {
            if (r.startLine == 0 && r.endLine == 3)
            {
                foundBlockComment = true;
            }
            if (r.startLine == 5 && r.endLine == 7)
            {
                foundContiguousComment = true;
            }
            if (r.startLine == 9 && r.endLine == 9)
            {
                foundSingleLineCommentFold = true;
            }
        }
    }

    CHECK(foundBlockComment);
    CHECK(foundContiguousComment);
    CHECK_FALSE(foundSingleLineCommentFold);
}

TEST_CASE("FoldingRanges - Preprocessor Directives and Regions")
{
    std::string code =
        "#region Initialization\n"
        "void Init() {\n"
        "    int x = 10;\n"
        "}\n"
        "#endregion\n"
        "\n"
        "#if DEBUG\n"
        "void DebugLog() {\n"
        "    // Log\n"
        "}\n"
        "#endif\n"
        "\n"
        "#include \"file1.as\"\n"
        "#include \"file2.as\"\n"
        "#include \"file3.as\"\n";

    FoldingTestEnv env(code);
    auto ranges = env.GetRanges();
    REQUIRE(ranges.has_value());

    bool foundRegion = false;
    bool foundIfDirective = false;
    bool foundImports = false;

    for (const auto &r : *ranges)
    {
        if (r.startLine == 0 && r.endLine == 4 && r.kind == lsp::FoldingRangeKind::Region)
        {
            foundRegion = true;
        }
        if (r.startLine == 6 && r.endLine == 10)
        {
            foundIfDirective = true;
        }
        if (r.startLine == 12 && r.endLine == 14 && r.kind == lsp::FoldingRangeKind::Imports)
        {
            foundImports = true;
        }
    }

    CHECK(foundRegion);
    CHECK(foundIfDirective);
    CHECK(foundImports);
}

TEST_CASE("FoldingRanges - Nested Regions and Multiline Lists")
{
    std::string code =
        "#region Outer\n"
        "#region Inner\n"
        "void Process(\n"
        "    int a,\n"
        "    int b,\n"
        "    int c\n"
        ") {\n"
        "}\n"
        "#endregion\n"
        "#endregion\n";

    FoldingTestEnv env(code);
    auto ranges = env.GetRanges();
    REQUIRE(ranges.has_value());

    bool foundOuterRegion = false;
    bool foundInnerRegion = false;
    bool foundParamList = false;

    for (const auto &r : *ranges)
    {
        if (r.startLine == 0 && r.endLine == 9 && r.kind == lsp::FoldingRangeKind::Region)
        {
            foundOuterRegion = true;
        }
        if (r.startLine == 1 && r.endLine == 8 && r.kind == lsp::FoldingRangeKind::Region)
        {
            foundInnerRegion = true;
        }
        if (r.startLine == 2 && r.endLine == 6)
        {
            foundParamList = true;
        }
    }

    CHECK(foundOuterRegion);
    CHECK(foundInnerRegion);
    CHECK(foundParamList);
}

TEST_CASE("FoldingRanges - Single-Line Functions and Empty Document")
{
    SUBCASE("Single-line function is omitted")
    {
        std::string code = "void inlineFunc() { return; }\n";
        FoldingTestEnv env(code);
        auto ranges = env.GetRanges();
        REQUIRE(ranges.has_value());
        // No multiline constructs, should be empty
        CHECK(ranges->empty());
    }

    SUBCASE("Empty document returns nullopt or empty")
    {
        std::string code = "";
        FoldingTestEnv env(code);
        auto ranges = env.GetRanges();
        CHECK(!ranges.has_value());
    }
}
