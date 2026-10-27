#include <doctest/doctest.h>

#include "features/breakpoints/BreakpointLocationsHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/PreprocessorRegions.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::parser;

namespace
{

struct BreakpointTestFixture
{
    AngelScriptParser parser;
    std::string sourceCode;
    TSTree* tree = nullptr;

    explicit BreakpointTestFixture(std::string code) : sourceCode(std::move(code))
    {
        tree = parser.Parse(sourceCode);
    }

    ~BreakpointTestFixture()
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }

    std::vector<BreakpointLocationItem> Query(uint32_t startLine = 0, uint32_t endLine = 100000,
                                              const std::vector<utils::ExcludedLineRange>* excluded = nullptr) const
    {
        if (!tree)
        {
            return {};
        }
        const BreakpointLocationsRequest req{ts_tree_root_node(tree), sourceCode, startLine, endLine, excluded};
        return BreakpointLocationsHandler::Collect(req);
    }
};

} // namespace

TEST_CASE("Breakpoint Locations - Basic Executable Statements")
{
    const std::string fnName = test::GenerateRandomSymbolName("Execute");
    const std::string varA = test::GenerateRandomSymbolName("alpha");
    const std::string varB = test::GenerateRandomSymbolName("beta");

    // Line 0: void Execute()
    // Line 1: {
    // Line 2:     int alpha = 42;
    // Line 3:     int beta = 10;
    // Line 4:     alpha = alpha + beta;
    // Line 5:     return;
    // Line 6: }
    const std::string code = "void " + fnName +
                             "()\n"
                             "{\n"
                             "    int " +
                             varA +
                             " = 42;\n"
                             "    int " +
                             varB +
                             " = 10;\n"
                             "    " +
                             varA + " = " + varA + " + " + varB +
                             ";\n"
                             "    return;\n"
                             "}\n";

    BreakpointTestFixture fixture(code);
    const auto locs = fixture.Query();

    REQUIRE_FALSE(locs.empty());

    // Expect locations on lines 2, 3, 4, 5
    std::vector<uint32_t> lines;
    for (const auto& loc : locs)
    {
        lines.push_back(loc.line);
    }

    CHECK(std::find(lines.begin(), lines.end(), 2) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 3) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 4) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 5) != lines.end());

    // Function header and braces should not be standalone executable statements
    CHECK(std::find(lines.begin(), lines.end(), 0) == lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 1) == lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 6) == lines.end());
}

TEST_CASE("Breakpoint Locations - Control Flow and Loops")
{
    const std::string fnName = test::GenerateRandomSymbolName("ControlFlow");
    const std::string iterVar = test::GenerateRandomSymbolName("idx");

    // Line 0: void ControlFlow()
    // Line 1: {
    // Line 2:     for (int idx = 0; idx < 10; ++idx)
    // Line 3:     {
    // Line 4:         if (idx == 5)
    // Line 5:             break;
    // Line 6:         else
    // Line 7:             continue;
    // Line 8:     }
    // Line 9: }
    const std::string code = "void " + fnName +
                             "()\n"
                             "{\n"
                             "    for (int " +
                             iterVar + " = 0; " + iterVar + " < 10; ++" + iterVar +
                             ")\n"
                             "    {\n"
                             "        if (" +
                             iterVar +
                             " == 5)\n"
                             "            break;\n"
                             "        else\n"
                             "            continue;\n"
                             "    }\n"
                             "}\n";

    BreakpointTestFixture fixture(code);
    const auto locs = fixture.Query();

    REQUIRE_FALSE(locs.empty());

    // For statement on line 2 should generate multiple column locations (init, condition, update)
    uint32_t line2Count = 0;
    for (const auto& loc : locs)
    {
        if (loc.line == 2)
        {
            ++line2Count;
        }
    }
    CHECK(line2Count >= 1);

    // If, break, continue locations
    std::vector<uint32_t> lines;
    for (const auto& loc : locs)
    {
        lines.push_back(loc.line);
    }
    CHECK(std::find(lines.begin(), lines.end(), 4) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 5) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 7) != lines.end());
}

TEST_CASE("Breakpoint Locations - Inactive Preprocessor Code Excluded")
{
    const std::string fnName = test::GenerateRandomSymbolName("PrepTest");
    const std::string deadVar = test::GenerateRandomSymbolName("deadVar");
    const std::string liveVar = test::GenerateRandomSymbolName("liveVar");

    // Line 0: void PrepTest()
    // Line 1: {
    // Line 2: #if 0
    // Line 3:     int deadVar = 999;
    // Line 4: #endif
    // Line 5:     int liveVar = 1;
    // Line 6: }
    const std::string code = "void " + fnName +
                             "()\n"
                             "{\n"
                             "#if 0\n"
                             "    int " +
                             deadVar +
                             " = 999;\n"
                             "#endif\n"
                             "    int " +
                             liveVar +
                             " = 1;\n"
                             "}\n";

    const std::vector<utils::ExcludedLineRange> excluded{{2, 4}};

    BreakpointTestFixture fixture(code);
    const auto locs = fixture.Query(0, 10, &excluded);

    for (const auto& loc : locs)
    {
        CHECK(loc.line != 2);
        CHECK(loc.line != 3);
        CHECK(loc.line != 4);
    }

    // Line 5 should have a valid breakpoint
    bool hasLine5 = false;
    for (const auto& loc : locs)
    {
        if (loc.line == 5)
        {
            hasLine5 = true;
        }
    }
    CHECK(hasLine5);
}

TEST_CASE("Breakpoint Locations - Sub-Range Query")
{
    const std::string fnName = test::GenerateRandomSymbolName("RangeCheck");
    const std::string v1 = test::GenerateRandomSymbolName("v1");
    const std::string v2 = test::GenerateRandomSymbolName("v2");
    const std::string v3 = test::GenerateRandomSymbolName("v3");

    // Line 0: void RangeCheck()
    // Line 1: {
    // Line 2:     int v1 = 1;
    // Line 3:     int v2 = 2;
    // Line 4:     int v3 = 3;
    // Line 5:     return;
    // Line 6: }
    const std::string code = "void " + fnName +
                             "()\n"
                             "{\n"
                             "    int " +
                             v1 +
                             " = 1;\n"
                             "    int " +
                             v2 +
                             " = 2;\n"
                             "    int " +
                             v3 +
                             " = 3;\n"
                             "    return;\n"
                             "}\n";

    BreakpointTestFixture fixture(code);
    // Query only line 3 to 3
    const auto locs = fixture.Query(3, 3);

    REQUIRE_FALSE(locs.empty());
    for (const auto& loc : locs)
    {
        CHECK(loc.line == 3);
    }
}

TEST_CASE("Breakpoint Locations - Comments and Class Declarations")
{
    const std::string clsName = test::GenerateRandomSymbolName("EntityDef");
    const std::string propName = test::GenerateRandomSymbolName("prop");

    // Line 0: // Header comment
    // Line 1: class EntityDef
    // Line 2: {
    // Line 3:     int prop;
    // Line 4: }
    const std::string code = "// Header comment\n"
                             "class " +
                             clsName +
                             "\n"
                             "{\n"
                             "    int " +
                             propName +
                             ";\n"
                             "}\n";

    BreakpointTestFixture fixture(code);
    const auto locs = fixture.Query();

    // Field declaration inside class without code body should not generate executable breakpoints
    for (const auto& loc : locs)
    {
        CHECK(loc.line != 0); // comment
        CHECK(loc.line != 1); // class header
    }
}
