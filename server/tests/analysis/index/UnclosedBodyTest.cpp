#include "analysis/diagnostics/Diagnostics.h"
#include "analysis/index/CallContextFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/Utils.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

TEST_SUITE("analysis/index/UnclosedBody")
{
    /** @brief Counts the "missing '}'" diagnostics in a collection result. */
    static size_t CountMissingBraceDiagnostics(const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics)
    {
        return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                                 [](const angel_lsp::analysis::Diagnostic& d)
                                                 { return d.message.find("missing '}'") != std::string::npos; }));
    }

    TEST_CASE("UnclosedBody - A file whose bodies are all closed reports no missing brace")
    {
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Closed");
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("run");
        const std::string code = "class " + cls + " {\n    void " + fn + "() { int a = 1; }\n}\n";

        angel_lsp::analysis::SymbolTable table;
        angel_lsp::parser::AngelScriptParser parser;
        angel_lsp::analysis::SymbolCollector collector(nullptr);
        const auto diagnostics = collector.CollectSymbols("file:///closed.as", code, parser, table);

        CHECK(CountMissingBraceDiagnostics(diagnostics) == 0);
        CHECK_FALSE(table.FindMemberSymbols(cls, fn).empty());
    }

    TEST_CASE("UnclosedBody - Class and method left open at the end of the file keep their symbols")
    {
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Open");
        const std::string field = angel_lsp::test::GenerateRandomSymbolName("count");
        const std::string method = angel_lsp::test::GenerateRandomSymbolName("run");
        const std::string lastLine = "        int local = 1;";
        const std::string code =
            "class " + cls + " {\n    int " + field + ";\n    void " + method + "() {\n" + lastLine + "\n";

        angel_lsp::analysis::SymbolTable table;
        angel_lsp::parser::AngelScriptParser parser;
        angel_lsp::analysis::SymbolCollector collector(nullptr);
        const auto diagnostics = collector.CollectSymbols("file:///open.as", code, parser, table);

        REQUIRE_FALSE(table.FindSymbols(cls).empty());
        CHECK_FALSE(table.FindMemberSymbols(cls, field).empty());
        const auto methods = table.FindMemberSymbols(cls, method);
        REQUIRE_FALSE(methods.empty());
        CHECK(methods[0].containerName == cls);

        // Both bodies close at the same byte, so the gap is named once, where the last token ends.
        REQUIRE(CountMissingBraceDiagnostics(diagnostics) == 1);
        const auto gap =
            std::find_if(diagnostics.begin(), diagnostics.end(), [](const angel_lsp::analysis::Diagnostic& d)
                         { return d.message.find("missing '}'") != std::string::npos; });
        CHECK(gap->range.start.line == 3);
        CHECK(gap->range.start.character == lastLine.size());
    }

    TEST_CASE("UnclosedBody - A half-typed call at the end of the file does not swallow the class around it")
    {
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Typing");
        const std::string callee = angel_lsp::test::GenerateRandomSymbolName("Deploy");
        const std::string method = angel_lsp::test::GenerateRandomSymbolName("Run");
        const std::string code = "class " + cls + " {\n    void " + callee + "(int a, int b) {}\n    void " + method +
                                 "() {\n        return " + callee + "(1, ";

        angel_lsp::analysis::SymbolTable table;
        angel_lsp::parser::AngelScriptParser parser;
        angel_lsp::analysis::SymbolCollector collector(nullptr);
        const auto diagnostics = collector.CollectSymbols("file:///typing.as", code, parser, table);

        CHECK_FALSE(table.FindMemberSymbols(cls, callee).empty());
        CHECK_FALSE(table.FindMemberSymbols(cls, method).empty());
        CHECK(CountMissingBraceDiagnostics(diagnostics) == 1);
        // Nothing before the half-typed statement is reported as broken.
        CHECK(std::none_of(diagnostics.begin(), diagnostics.end(),
                           [](const angel_lsp::analysis::Diagnostic& d) { return d.range.start.line < 3; }));

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        angel_lsp::analysis::NodeIndex index(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto ctx = angel_lsp::analysis::FindCallContext(index, static_cast<uint32_t>(code.size()));
        REQUIRE(ctx.has_value());
        CHECK(ctx->activeParameter == 1);
        CHECK(angel_lsp::utils::SliceSource(code, ctx->calleeStartByte, ctx->calleeEndByte) == callee);
        ts_tree_delete(tree);
    }
}
