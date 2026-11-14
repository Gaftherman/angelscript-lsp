#include "analysis/checkers/DefiniteAssignmentChecker.h"

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/index/DefiniteAssignmentFacts.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "helpers/CorpusDirectory.h"
#include "helpers/RuleCorpusAudit.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
std::vector<Diagnostic> AnalyzeCode(const std::string& code, const std::string& fileUri = "file:///definite_assign.as")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    collector.CollectSymbols(fileUri, code, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    TSTree* tree = parser.Parse(code);
    NodeIndex nodeIndex(ts_tree_root_node(tree));
    request.nodeIndex = &nodeIndex;

    SemanticAnalyzer analyzer(nullptr);
    auto diagnostics = analyzer.Analyze(request);

    if (tree)
    {
        ts_tree_delete(tree);
    }
    return diagnostics;
}

bool HasUninitializedRead(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d) { return d.code == "as-warn-uninitialized-variable-read"; });
}
} // namespace

static bool MatchesVerdict(const std::vector<Diagnostic>& diagnostics, const std::string& expectedVerdict)
{
    if (expectedVerdict == "none")
    {
        return !HasUninitializedRead(diagnostics);
    }
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d) { return d.code == expectedVerdict; });
}

TEST_SUITE("DefiniteAssignment")
{
    TEST_CASE("DefiniteAssignmentChecker - Direct Uninitialized Read")
    {
        std::string badCode = "void Print(int v) { }\n"
                              "void main()\n"
                              "{\n"
                              "    int x;\n"
                              "    Print(x);\n"
                              "}\n";
        CHECK(HasUninitializedRead(AnalyzeCode(badCode)));

        std::string goodCode = "void Print(int v) { }\n"
                               "void main()\n"
                               "{\n"
                               "    int x = 42;\n"
                               "    Print(x);\n"
                               "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(goodCode)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Recorded statement worklist reaches a deeply nested read")
    {
        constexpr size_t depth = 2048;
        std::string code = "int Deep() { int value; ";
        code.append(depth, '{');
        code.append("return value;");
        code.append(depth, '}');
        code.append("}");

        AngelScriptParser parser;
        const std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(parser.Parse(code), ts_tree_delete);
        REQUIRE(tree);
        const TSNode root = ts_tree_root_node(tree.get());
        const NodeIndex index(root);
        SymbolTable table;
        static angel_lsp::i18n::I18n i18n;
        const SemanticAnalysisRequest request{table, "file:///deep_assignment.as", ".as.predefined", &i18n};
        std::vector<Diagnostic> diagnostics;
        DiagnosticContext ctx(request, diagnostics);
        CheckDefiniteAssignment({code, nullptr, &index}, ctx);
        REQUIRE(diagnostics.size() == 1);
        CHECK(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("DefiniteAssignmentChecker - Recorded expression worklist reaches a deeply parenthesized read")
    {
        constexpr size_t depth = 3072;
        std::string code = "int Deep() { int value; return ";
        code.append(depth, '(');
        code.append("value");
        code.append(depth, ')');
        code.append("; }");

        AngelScriptParser parser;
        const std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(parser.Parse(code), ts_tree_delete);
        REQUIRE(tree);
        const TSNode root = ts_tree_root_node(tree.get());
        const NodeIndex index(root);
        SymbolTable table;
        static angel_lsp::i18n::I18n i18n;
        const SemanticAnalysisRequest request{table, "file:///deep_expression.as", ".as.predefined", &i18n};
        std::vector<Diagnostic> diagnostics;
        DiagnosticContext ctx(request, diagnostics);
        CheckDefiniteAssignment({code, nullptr, &index}, ctx);
        REQUIRE(diagnostics.size() == 1);
        CHECK(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("DefiniteAssignmentChecker - A long switch case visits every statement after its label")
    {
        std::string code = "int Read(int selector) { int value; switch (selector) { case 1:\n";
        for (size_t i = 0; i < 255; ++i)
        {
            code.append("    selector = 1;\n");
        }
        code.append("    return value;\n default: return 0; } }\n");
        CHECK(HasUninitializedRead(AnalyzeCode(code)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Branching If Else")
    {
        std::string assignedBoth = "void Print(int v) { }\n"
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

        std::string assignedOne = "void Print(int v) { }\n"
                                  "void main()\n"
                                  "{\n"
                                  "    int x;\n"
                                  "    bool cond = true;\n"
                                  "    if (cond)\n"
                                  "        x = 1;\n"
                                  "    Print(x);\n"
                                  "}\n";
        // Clean, and this test asserted the opposite. AngelScript's rule is not C#'s: it warns only
        // when NO assignment precedes the read, conditional or not. Measured on this exact snippet
        // - `angelscript_oracle` accepts it without a warning - and on `if (false) { x = 5; }
        // Print(x);`, which is clean too. The reverse order still warns: `Print(x); x = 5;` is
        // "'x' is not initialized."
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(assignedOne)));

        std::string readBeforeAnyAssignment = "void Print(int v) { }\n"
                                              "void main()\n"
                                              "{\n"
                                              "    int x;\n"
                                              "    Print(x);\n"
                                              "    x = 1;\n"
                                              "}\n";
        CHECK(HasUninitializedRead(AnalyzeCode(readBeforeAnyAssignment)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Early Return Path")
    {
        std::string earlyReturn = "void Print(int v) { }\n"
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

    TEST_CASE("DefiniteAssignmentChecker - Out Parameter Assignment")
    {
        std::string outParam = "void Init(int &out val) { val = 10; }\n"
                               "void Print(int v) { }\n"
                               "void main()\n"
                               "{\n"
                               "    int x;\n"
                               "    Init(x);\n"
                               "    Print(x);\n"
                               "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(outParam)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Recorded reads respect right hand sides and argument order")
    {
        const std::string code = "void ReadThenWrite(int input, int &out output) { output = input; }\n"
                                 "void WriteThenRead(int &out output, int input) { output = input; }\n"
                                 "void Run() {\n"
                                 "    int a, b, c, d;\n"
                                 "    ReadThenWrite(a, a);\n"
                                 "    WriteThenRead(b, b);\n"
                                 "    c = c;\n"
                                 "    d = (d = 1);\n"
                                 "}\n";
        std::vector<uint32_t> warningLines;
        for (const auto& diagnostic : AnalyzeCode(code))
        {
            if (diagnostic.code == "as-warn-uninitialized-variable-read")
            {
                warningLines.push_back(diagnostic.range.start.line);
            }
        }
        CHECK(warningLines == std::vector<uint32_t>{4, 6});
    }

    TEST_CASE("DefiniteAssignmentChecker - Loops Dataflow")
    {
        std::string doWhileCode = "void Print(int v) { }\n"
                                  "void main()\n"
                                  "{\n"
                                  "    int x;\n"
                                  "    do {\n"
                                  "        x = 10;\n"
                                  "    } while (false);\n"
                                  "    Print(x);\n"
                                  "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(doWhileCode)));

        std::string whileCode = "void Print(int v) { }\n"
                                "void main()\n"
                                "{\n"
                                "    int x;\n"
                                "    bool cond = true;\n"
                                "    while (cond) {\n"
                                "        x = 10;\n"
                                "    }\n"
                                "    Print(x);\n"
                                "}\n";
        // Clean. `while (cond) { x = 10; } Print(x);` is accepted by the compiler even though
        // the loop may not run - measured.
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(whileCode)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Switch Statement Dataflow")
    {
        std::string switchAll = "void Print(int v) { }\n"
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

        std::string switchNoDefault = "void Print(int v) { }\n"
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
        // Clean. A missing `default:` decides "assigned on every path", which is C#'s rule;
        // AngelScript asks only that some assignment precede the read. Measured.
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(switchNoDefault)));
    }
}

// =====================================================================================
// DEFINITE ASSIGNMENT CORPUS AUDIT (skip()-decorated, run it deliberately:
// `angel_lsp_tests.exe --no-skip --test-case="*Definite Assignment Corpus Audit*"`)
//
// This rule had no corpus measurement at all, and it needed one: `as-err-uninitialized-variable-
// read` is an ERROR on a shape - reading a local before writing it - that legal code produces
// constantly through `&out` parameters, which are AngelScript's only way to return a second value.
//
// The gap was not theoretical. Every out-parameter of every METHOD was mis-read as a read, because
// the candidate lookup resolved the receiver's type against the ROOT scope and a local receiver is
// never there. `int n; reader.Get(n);` - the line that initialises `n` - was reported as using it
// uninitialised. Free functions were unaffected, which is why unit tests never caught it: they are
// what a unit test reaches for. It took doc_p28, written for something else entirely.
// =====================================================================================

TEST_CASE("DefiniteAssignment - Definite Assignment Corpus Audit" * doctest::skip(true))
{
    if (!angel_lsp::test::CorpusIsAvailable())
    {
        MESSAGE(angel_lsp::test::CorpusMissingMessage());
        return;
    }

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string& code)
                                                        { return code == "as-warn-uninitialized-variable-read"; });

    MESSAGE("Definite-assignment corpus audit: files=" << result.filesAnalysed << " totalFlagged=" << result.Total()
                                                       << " seconds=" << result.seconds);

    for (const auto& hit : result.hits)
    {
        MESSAGE("  " << hit.fileName << ":" << hit.line << " [" << hit.code << "] " << hit.message);
    }

    CHECK(result.filesAnalysed > 0);

    // 749 when this audit was first written, 7 now. Five separate causes, each measured against
    // the compiler rather than reasoned about:
    //
    //   the rule itself      "definitely assigned on every path" is C#'s rule; AngelScript warns
    //                        only when NO assignment precedes the read, and it is a warning
    //   loop bodies          an assignment inside a loop survives it
    //   `if`/`while`/`do`/`switch` conditions were never analysed at all - no `condition` field
    //   `for` headers        the field is `init`, not `initializer`
    //   unknown callees      an `&out` argument cannot be told from a read when the callee is
    //                        invisible, and `&out` is what it usually is
    //
    // A sixth cause was found after those five, and it took the count from 7 to 1: an `&out`
    // parameter was read off whichever overload resolution happened to pick, and picking one is a
    // guess whenever the candidates disagree. The corpus shape is a subclass restating its base's
    // methods, so the lookup offers two declarations of one method and there is no reason to
    // prefer either; reading `&out` off the loser reported the line that INITIALISES the variable.
    // It now asks whether ANY candidate declares an out-parameter at that position.
    //
    // The one finding left is `angelscript_clean_examples.as:2269`, and it is exactly the finding
    // the compiler makes on that line. Six more the compiler reports are missed here, which is the
    // safe direction.
    //
    // The count is a ratchet and may only go down.
    constexpr size_t k_accountedFindings = 1;
    CHECK(result.Total() <= k_accountedFindings);
}

TEST_SUITE("DefiniteAssignmentVariations")
{
    TEST_CASE("DefiniteAssignmentChecker - Out param assigned in nested if within for loop read after loop")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 42; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    for (int " +
                                 iVar + " = 0; " + iVar + " < 5; ++" + iVar +
                                 ")\n"
                                 "    {\n"
                                 "        if (" +
                                 iVar +
                                 " == 2)\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "    " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("DefiniteAssignmentChecker - Out param read inside nested if within for loop before assignment")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 42; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    for (int " +
                                 iVar + " = 0; " + iVar + " < 5; ++" + iVar +
                                 ")\n"
                                 "    {\n"
                                 "        if (" +
                                 iVar +
                                 " == 2)\n"
                                 "        {\n"
                                 "            " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-uninitialized-variable-read";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasUninitializedRead(diagnostics));
    }

    TEST_CASE(
        "DefiniteAssignmentChecker - Out param assigned in nested for loops in both if and else branches read after")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string flagParam = angel_lsp::test::GenerateRandomSymbolName("flag");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string jVar = angel_lsp::test::GenerateRandomSymbolName("j");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 100; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn + "(bool " + flagParam +
                                 ")\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    if (" +
                                 flagParam +
                                 ")\n"
                                 "    {\n"
                                 "        for (int " +
                                 iVar + " = 0; " + iVar + " < 2; ++" + iVar +
                                 ")\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "    else\n"
                                 "    {\n"
                                 "        for (int " +
                                 jVar + " = 0; " + jVar + " < 3; ++" + jVar +
                                 ")\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "    " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("DefiniteAssignmentChecker - Out param assigned in nested do-while within if block read after do-while")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string condParam = angel_lsp::test::GenerateRandomSymbolName("cond");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 99; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn + "(bool " + condParam +
                                 ")\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    if (" +
                                 condParam +
                                 ")\n"
                                 "    {\n"
                                 "        do\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        } while (false);\n"
                                 "        " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }

    TEST_CASE(
        "DefiniteAssignmentChecker - Out param read in for loop condition inside switch case before body assignment")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string optionParam = angel_lsp::test::GenerateRandomSymbolName("option");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 1; }\n"
                                 "void " +
                                 testFn + "(int " + optionParam +
                                 ")\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    switch (" +
                                 optionParam +
                                 ")\n"
                                 "    {\n"
                                 "    case 1:\n"
                                 "        for (int " +
                                 iVar + " = 0; " + iVar + " < " + dataVar + "; ++" + iVar +
                                 ")\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "        break;\n"
                                 "    default:\n"
                                 "        break;\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-uninitialized-variable-read";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("DefiniteAssignmentChecker - Out param assigned in while condition expression read inside nested if")
    {
        const std::string tryFetchFn = angel_lsp::test::GenerateRandomSymbolName("TryFetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "bool " + tryFetchFn + "(int &out " + targetParam +
                                 ")\n"
                                 "{\n"
                                 "    " +
                                 targetParam +
                                 " = 42;\n"
                                 "    return true;\n"
                                 "}\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    while (" +
                                 tryFetchFn + "(" + dataVar +
                                 "))\n"
                                 "    {\n"
                                 "        if (" +
                                 dataVar +
                                 " > 0)\n"
                                 "        {\n"
                                 "            " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "        break;\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("DefiniteAssignmentChecker - Out param read in outer for loop before nested inner for loop assignment")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string jVar = angel_lsp::test::GenerateRandomSymbolName("j");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 5; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    for (int " +
                                 iVar + " = 0; " + iVar + " < 3; ++" + iVar +
                                 ")\n"
                                 "    {\n"
                                 "        " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "        for (int " +
                                 jVar + " = 0; " + jVar + " < 3; ++" + jVar +
                                 ")\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-uninitialized-variable-read";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasUninitializedRead(diagnostics));
    }

    TEST_CASE(
        "DefiniteAssignmentChecker - Out param assigned across all switch cases inside do-while read after switch")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string tagParam = angel_lsp::test::GenerateRandomSymbolName("tag");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 12; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn + "(int " + tagParam +
                                 ")\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    do\n"
                                 "    {\n"
                                 "        switch (" +
                                 tagParam +
                                 ")\n"
                                 "        {\n"
                                 "        case 1:\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "            break;\n"
                                 "        default:\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "            break;\n"
                                 "        }\n"
                                 "        " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "    } while (false);\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }
}

TEST_SUITE("DefiniteAssignmentConditions")
{
    // The condition of an `if`, `while`, `do`/`while` and `switch` carries NO field name in the
    // grammar - the expression is an unnamed child. This checker asked for a "condition" field in
    // all four, got null every time, and so never analysed a condition at all. Both halves were
    // wrong: a read inside a condition went unchecked, and an `&out` argument written there never
    // marked its variable assigned, which then reported the body.
    //
    // `for` was the same story under a different name: its field is `init`, not `initializer`, so
    // the loop header was skipped and `for (i = 0; i < n; i++)` reported the `i` in its own
    // condition. Together these were 195 of the 749 findings this rule produced over the corpus.

    TEST_CASE("DefiniteAssignmentChecker - A read inside a condition is checked")
    {
        CHECK(HasUninitializedRead(AnalyzeCode("void main() { int x; if (x > 0) { } }\n")));

        CHECK(HasUninitializedRead(AnalyzeCode("void main() { int x; while (x > 0) { break; } }\n")));

        CHECK(HasUninitializedRead(AnalyzeCode("void main() { int x; switch (x) { case 1: break; } }\n")));

        CHECK(HasUninitializedRead(AnalyzeCode("void main() { int x; do { break; } while (x > 0); }\n")));
    }

    TEST_CASE("DefiniteAssignmentChecker - An out-argument written in a condition counts for the body after it")
    {
        // The corpus shape: `if (dict.get(key, value) && value != 0) { use(value); }`. The callee
        // is invisible, so whether that parameter is `&out` cannot be established - and `&out` is
        // what it usually is, so the variable is treated as possibly written rather than read.
        CHECK_FALSE(HasUninitializedRead(
            AnalyzeCode("void Use(int v) { }\n"
                        "void main() { UnknownDict d; int v; if (d.get('k', v) && v != 0) { Use(v); } }\n")));

        CHECK_FALSE(HasUninitializedRead(AnalyzeCode("void Use(int v) { }\n"
                                                     "void main() { int v; if (HostGet('k', v)) { Use(v); } }\n")));
    }

    TEST_CASE("DefiniteAssignmentChecker - A for loop header initialises its own counter")
    {
        // `for (i = 0; ...)` assigns a variable declared earlier. The header is where `i` becomes
        // initialised, and it was being reported as reading it uninitialised.
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode("void Use(int v) { }\n"
                                                     "void main() { int i; for (i = 0; i < 3; i++) { Use(i); } }\n")));

        CHECK_FALSE(HasUninitializedRead(AnalyzeCode("void Use(int v) { }\n"
                                                     "void main() { for (int i = 0; i < 3; i++) { Use(i); } }\n")));
    }
}

TEST_SUITE("DefiniteAssignmentOverloads")
{
    TEST_CASE("DefiniteAssignmentChecker - An out-parameter is recognised when any candidate declares one")
    {
        // Overload resolution picks one candidate; reading `&out` off that pick is a guess whenever
        // the candidates disagree, and a wrong guess reports the line that INITIALISES the
        // variable. Asking "could this be an out-parameter" instead of "is the chosen one" is the
        // same silence-over-guessing the unknown-callee case uses.
        CHECK_FALSE(
            HasUninitializedRead(AnalyzeCode("void Use(int v) { }\n"
                                             "class B { bool Get(int &out v) { v = 1; return true; } }\n"
                                             "class D : B { bool Get(int &out v) override { v = 2; return true; } }\n"
                                             "void main() { D d; int n; d.Get(n); Use(n); }\n")));

        // Still reported where NO candidate declares an out-parameter at that position.
        CHECK(HasUninitializedRead(AnalyzeCode("void Use(int v) { }\n"
                                               "class B { bool Get(int v) { return true; } }\n"
                                               "void main() { B b; int n; b.Get(n); }\n")));
    }
}

TEST_SUITE("DefiniteAssignmentStatementOrder")
{
    // Every statement record slices the call/read/assignment arrays by the position it started
    // appending at. When the arrays were sorted after the records were built, an `&out` call and
    // the read that follows it in a loop body were attributed to the wrong statements, and a
    // `float x, y;` filled by `Spread(x, y)` was reported at its first use.
    static const char* const k_spreadStub = "class Util { void Spread(float &out a, float &out b) const { } }\n"
                                            "Util g_Util;\n"
                                            "float Scale(float v) { return v; }\n";

    TEST_CASE("DefiniteAssignmentChecker - Good: an out call inside a loop initialises the variables the next "
              "statement reads")
    {
        const std::string code = std::string(k_spreadStub) + "void Fire(uint shots)\n"
                                                             "{\n"
                                                             "    float x, y;\n"
                                                             "    for (uint i = 0; i < shots; ++i)\n"
                                                             "    {\n"
                                                             "        g_Util.Spread(x, y);\n"
                                                             "        float dx = Scale(x) + y * 2.0f;\n"
                                                             "    }\n"
                                                             "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(code)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Good: the same shape inside a mixin method")
    {
        const std::string code = std::string(k_spreadStub) + "mixin class Shooter\n"
                                                             "{\n"
                                                             "    void Fire(uint shots)\n"
                                                             "    {\n"
                                                             "        float x, y;\n"
                                                             "        int tag = 0;\n"
                                                             "        for (uint i = 0; i < shots; ++i)\n"
                                                             "        {\n"
                                                             "            g_Util.Spread(x, y);\n"
                                                             "            float dx = x + y;\n"
                                                             "        }\n"
                                                             "    }\n"
                                                             "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(code)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Bad: a read before any out call is still reported")
    {
        const std::string code = std::string(k_spreadStub) + "void Fire(uint shots)\n"
                                                             "{\n"
                                                             "    float x, y;\n"
                                                             "    for (uint i = 0; i < shots; ++i)\n"
                                                             "    {\n"
                                                             "        float dx = x + y;\n"
                                                             "        g_Util.Spread(x, y);\n"
                                                             "    }\n"
                                                             "}\n";
        CHECK(HasUninitializedRead(AnalyzeCode(code)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Neutral: an out call in one branch does not hide a read in the loop that "
              "never ran it")
    {
        const std::string code = std::string(k_spreadStub) + "void Fire(bool aim)\n"
                                                             "{\n"
                                                             "    float x, y;\n"
                                                             "    if (aim) { g_Util.Spread(x, y); }\n"
                                                             "    float dx = Scale(x);\n"
                                                             "}\n";
        // AngelScript warns only when NO assignment precedes the read; one in a branch counts.
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(code)));
    }

    TEST_CASE("DefiniteAssignmentChecker - Invariant: every fact slice of a record lies inside the record")
    {
        const std::string code = std::string(k_spreadStub) +
                                 "void Fire(uint shots, bool a)\n"
                                 "{\n"
                                 "    float x, y;\n"
                                 "    float z = Scale(1.0f);\n"
                                 "    for (uint i = 0; i < shots; ++i)\n"
                                 "    {\n"
                                 "        g_Util.Spread(x, y);\n"
                                 "        if (a) { z = Scale(x); } else { z = y; }\n"
                                 "        float w = Scale(z) + x;\n"
                                 "        while (w > 0.0f) { w = Scale(w) - 1.0f; }\n"
                                 "    }\n"
                                 "    switch (shots) { case 1: z = Scale(y); break; default: break; }\n"
                                 "}\n";
        AngelScriptParser parser;
        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        NodeIndex nodeIndex(ts_tree_root_node(tree));
        const DefiniteAssignmentFacts facts = BuildDefiniteAssignmentFacts(nodeIndex, code);
        REQUIRE(!facts.records.empty());
        for (const auto& rec : facts.records)
        {
            for (const auto& call : facts.CallsOf(rec))
            {
                CHECK(call.bytes.startByte >= rec.bytes.startByte);
                CHECK(call.bytes.endByte <= rec.bytes.endByte);
            }
            for (const auto& read : facts.ReadsOf(rec))
            {
                CHECK(read.bytes.startByte >= rec.bytes.startByte);
                CHECK(read.bytes.endByte <= rec.bytes.endByte);
            }
            for (const auto& decl : facts.DeclaratorsOf(rec))
            {
                CHECK(decl.bytes.startByte >= rec.bytes.startByte);
                CHECK(decl.bytes.endByte <= rec.bytes.endByte);
            }
        }
        ts_tree_delete(tree);
    }
}
