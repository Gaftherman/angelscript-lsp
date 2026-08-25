#include <doctest/doctest.h>
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include <algorithm>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    std::vector<Diagnostic> AnalyzeCode(const std::string &code, const std::string &fileUri = "file:///definite_assign.as")
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static angel_lsp::i18n::I18n i18n;

        collector.CollectSymbols(fileUri, code, parser, table);

        SemanticAnalysisRequest request{ table, fileUri, ".as.predefined", &i18n };
        request.scopeRoot = scopes.CollectScopes(code, parser);
        request.sourceCode = code;
        request.tree = parser.Parse(code);

        SemanticAnalyzer analyzer(nullptr);
        auto diagnostics = analyzer.Analyze(request);

        if (request.tree)
        {
            ts_tree_delete(const_cast<TSTree *>(request.tree));
        }
        return diagnostics;
    }

    bool HasUninitializedRead(const std::vector<Diagnostic> &diagnostics)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [](const Diagnostic &d) { return d.code == "as-err-uninitialized-variable-read"; });
    }
}

TEST_SUITE("DefiniteAssignment")
{
    TEST_CASE("Direct Uninitialized Read")
    {
        std::string badCode =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    Print(x);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCode(badCode)));

        std::string goodCode =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x = 42;\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(goodCode)));
    }

    TEST_CASE("Branching If Else")
    {
        std::string assignedBoth =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool cond = true;\n"
            "    if (cond)\n"
            "        x = 1;\n"
            "    else\n"
            "        x = 2;\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(assignedBoth)));

        std::string assignedOne =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool cond = true;\n"
            "    if (cond)\n"
            "        x = 1;\n"
            "    Print(x);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCode(assignedOne)));
    }

    TEST_CASE("Early Return Path")
    {
        std::string earlyReturn =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool cond = false;\n"
            "    if (!cond)\n"
            "        return;\n"
            "    x = 100;\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(earlyReturn)));
    }

    TEST_CASE("Out Parameter Assignment")
    {
        std::string outParam =
            "void Init(int &out val) { val = 10; }\n"
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    Init(x);\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(outParam)));
    }

    TEST_CASE("Loops Dataflow")
    {
        std::string doWhileCode =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    do {\n"
            "        x = 10;\n"
            "    } while (false);\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(doWhileCode)));

        std::string whileCode =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool cond = true;\n"
            "    while (cond) {\n"
            "        x = 10;\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCode(whileCode)));
    }

    TEST_CASE("Switch Statement Dataflow")
    {
        std::string switchAll =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    int mode = 1;\n"
            "    switch (mode) {\n"
            "    case 1:\n"
            "        x = 10;\n"
            "        break;\n"
            "    default:\n"
            "        x = 20;\n"
            "        break;\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(switchAll)));

        std::string switchNoDefault =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    int mode = 1;\n"
            "    switch (mode) {\n"
            "    case 1:\n"
            "        x = 10;\n"
            "        break;\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCode(switchNoDefault)));
    }
}
