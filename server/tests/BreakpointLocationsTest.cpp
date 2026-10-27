#include <doctest/doctest.h>

#include "features/breakpoints/BreakpointLocationsHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/PreprocessorRegions.h"

#include <angel_lsp/angel_lsp_debugger.h>
#include <angelscript.h>

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
        const BreakpointLocationsRequest req{ts_tree_root_node(tree), startLine, endLine, excluded};
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

    // Expect locations on lines 2, 3, 4, 5, and function closing brace on line 6
    std::vector<uint32_t> lines;
    for (const auto& loc : locs)
    {
        lines.push_back(loc.line);
    }

    CHECK(std::find(lines.begin(), lines.end(), 2) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 3) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 4) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 5) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 6) != lines.end());

    // Function header and opening brace should not be standalone executable statements
    CHECK(std::find(lines.begin(), lines.end(), 0) == lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 1) == lines.end());
}

TEST_CASE("Breakpoint Locations - Uninitialized Variables vs Initialized")
{
    const std::string nsName = test::GenerateRandomSymbolName("models");
    const std::string uninitGlobal = test::GenerateRandomSymbolName("laserbeam");
    const std::string initGlobal = test::GenerateRandomSymbolName("maxSpread");
    const std::string clsName = test::GenerateRandomSymbolName("WeaponConfig");
    const std::string uninitField = test::GenerateRandomSymbolName("damage");
    const std::string initField = test::GenerateRandomSymbolName("ammo");
    const std::string fnName = test::GenerateRandomSymbolName("Setup");
    const std::string uninitLocal = test::GenerateRandomSymbolName("temp");
    const std::string initLocal = test::GenerateRandomSymbolName("count");

    // Line 0: namespace models {
    // Line 1:     uint laserbeam;
    // Line 2:     int maxSpread = 100;
    // Line 3: }
    // Line 4: class WeaponConfig {
    // Line 5:     float damage;
    // Line 6:     int ammo = 30;
    // Line 7:     void Setup() {
    // Line 8:         int temp;
    // Line 9:         int count = 5;
    // Line 10:    }
    // Line 11: }
    const std::string code = "namespace " + nsName +
                             " {\n"
                             "    uint " +
                             uninitGlobal +
                             ";\n"
                             "    int " +
                             initGlobal +
                             " = 100;\n"
                             "}\n"
                             "class " +
                             clsName +
                             " {\n"
                             "    float " +
                             uninitField +
                             ";\n"
                             "    int " +
                             initField +
                             " = 30;\n"
                             "    void " +
                             fnName +
                             "() {\n"
                             "        int " +
                             uninitLocal +
                             ";\n"
                             "        int " +
                             initLocal +
                             " = 5;\n"
                             "    }\n"
                             "}\n";

    BreakpointTestFixture fixture(code);
    const auto locs = fixture.Query();

    std::vector<uint32_t> lines;
    for (const auto& loc : locs)
    {
        lines.push_back(loc.line);
    }

    // Uninitialized namespace global should NOT have a breakpoint
    CHECK(std::find(lines.begin(), lines.end(), 1) == lines.end());
    // Initialized namespace global SHOULD have a breakpoint
    CHECK(std::find(lines.begin(), lines.end(), 2) != lines.end());

    // Uninitialized class member should NOT have a breakpoint
    CHECK(std::find(lines.begin(), lines.end(), 5) == lines.end());
    // Initialized class member SHOULD have a breakpoint
    CHECK(std::find(lines.begin(), lines.end(), 6) != lines.end());

    // Uninitialized local primitive should NOT have a breakpoint (as_bytecode.cpp:1184 optimizer strips asBC_LINE)
    CHECK(std::find(lines.begin(), lines.end(), 8) == lines.end());
    // Initialized local primitive SHOULD have a breakpoint
    CHECK(std::find(lines.begin(), lines.end(), 9) != lines.end());
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

    // For statement on line 2 should generate column locations for header, init, condition, update
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

    // Verify compound statement line range does not span into child lines
    for (const auto& loc : locs)
    {
        if (loc.line == 4) // if statement
        {
            CHECK(loc.endLine == 4);
        }
    }
}

TEST_CASE("Breakpoint Locations - Do While Loop and Closing Brace")
{
    const std::string fnName = test::GenerateRandomSymbolName("DoWhileTest");
    const std::string countVar = test::GenerateRandomSymbolName("cnt");

    // Line 0: void DoWhileTest()
    // Line 1: {
    // Line 2:     int cnt = 0;
    // Line 3:     do
    // Line 4:     {
    // Line 5:         cnt++;
    // Line 6:     } while (cnt < 5);
    // Line 7: }
    const std::string code = "void " + fnName +
                             "()\n"
                             "{\n"
                             "    int " +
                             countVar +
                             " = 0;\n"
                             "    do\n"
                             "    {\n"
                             "        " +
                             countVar +
                             "++;\n"
                             "    } while (" +
                             countVar +
                             " < 5);\n"
                             "}\n";

    BreakpointTestFixture fixture(code);
    const auto locs = fixture.Query();

    std::vector<uint32_t> lines;
    for (const auto& loc : locs)
    {
        lines.push_back(loc.line);
    }

    // Both 'do' start (line 3) and 'while (cond)' end (line 6) must be valid breakpoint locations
    CHECK(std::find(lines.begin(), lines.end(), 3) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 5) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 6) != lines.end());
    // Function exit brace on line 7
    CHECK(std::find(lines.begin(), lines.end(), 7) != lines.end());
}

TEST_CASE("Breakpoint Locations - Empty For Loop Clauses")
{
    const std::string fnName = test::GenerateRandomSymbolName("InfiniteLoop");

    // Line 0: void InfiniteLoop()
    // Line 1: {
    // Line 2:     for (;;)
    // Line 3:     {
    // Line 4:         break;
    // Line 5:     }
    // Line 6: }
    const std::string code = "void " + fnName +
                             "()\n"
                             "{\n"
                             "    for (;;)\n"
                             "    {\n"
                             "        break;\n"
                             "    }\n"
                             "}\n";

    BreakpointTestFixture fixture(code);
    const auto locs = fixture.Query();

    // for (;;) should produce only 1 location on line 2 (the header itself, no bogus semicolon column breakpoints)
    uint32_t line2Count = 0;
    for (const auto& loc : locs)
    {
        if (loc.line == 2)
        {
            ++line2Count;
        }
    }
    CHECK(line2Count == 1);
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

    // Field declaration inside class without initializers produces 0 breakpoints
    CHECK(locs.empty());
}

TEST_CASE("Breakpoint Locations - Syntax Errors and Incomplete Code Resilience")
{
    const std::string fnName = test::GenerateRandomSymbolName("BrokenSyntax");
    const std::string goodVar = test::GenerateRandomSymbolName("goodVar");

    // Line 0: void BrokenSyntax()
    // Line 1: {
    // Line 2:     int bad = ;
    // Line 3:     ;
    // Line 4:     int goodVar = 42;
    // Line 5:     Foo(
    // Line 6: }
    const std::string code = "void " + fnName +
                             "()\n"
                             "{\n"
                             "    int bad = ;\n"
                             "    ;\n"
                             "    int " +
                             goodVar +
                             " = 42;\n"
                             "    Foo(\n"
                             "}\n";

    BreakpointTestFixture fixture(code);
    const auto locs = fixture.Query();

    // Valid statement on line 4 must still be discovered despite syntax errors on line 2, 3, 5
    bool foundGoodVar = false;
    for (const auto& loc : locs)
    {
        if (loc.line == 4)
        {
            foundGoodVar = true;
        }
        // Bare semicolon on line 3 must NOT produce a breakpoint location
        CHECK(loc.line != 3);
    }
    CHECK(foundGoodVar);
}

TEST_CASE("Debugger Runtime - Attach, Breakpoints, and Lifecycle")
{
    angel_lsp::debug::Debugger debugger;
    const std::string section = "test_script.as";
    debugger.SetBreakpoint(section, 10);
    debugger.SetBreakpoint(section, 20);

    debugger.ClearBreakpoint(section, 10);
    debugger.ClearAllBreakpoints();
    debugger.Continue();
    debugger.StepIn();
    debugger.Detach();
}