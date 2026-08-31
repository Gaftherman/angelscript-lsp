#include <doctest/doctest.h>
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include <algorithm>

#include "helpers/CorpusDirectory.h"
#include "helpers/RuleCorpusAudit.h"

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
                           [](const Diagnostic &d) { return d.code == "as-warn-uninitialized-variable-read"; });
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
        // Clean, and this test asserted the opposite. AngelScript's rule is not C#'s: it warns only
        // when NO assignment precedes the read, conditional or not. Measured on this exact snippet
        // - `angelscript_oracle` accepts it without a warning - and on `if (false) { x = 5; }
        // Print(x);`, which is clean too. The reverse order still warns: `Print(x); x = 5;` is
        // "'x' is not initialized."
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(assignedOne)));

        std::string readBeforeAnyAssignment =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    Print(x);\n"
            "    x = 1;\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCode(readBeforeAnyAssignment)));
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
        // Clean. `while (cond) { x = 10; } Print(x);` is accepted by the compiler even though
        // the loop may not run - measured.
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(whileCode)));
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

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return code == "as-warn-uninitialized-variable-read";
    });

    MESSAGE("Definite-assignment corpus audit: files=" << result.filesAnalysed
            << " totalFlagged=" << result.Total()
            << " seconds=" << result.seconds);

    for (const auto &hit : result.hits)
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
    // The compiler's own count over the same 1,061 files is also 7, and exactly one of them is the
    // same finding - `angelscript_clean_examples.as:2269`. The other six of ours are all
    // `value.Get(fvalue, strict)` in the JSON library, where the receiver's type IS visible and the
    // out-parameter should have been recognised; that overload set is the one ResolveBestOverload
    // also calls ambiguous 75 times, so the two are one defect and it is the first WIP item.
    // The six the compiler finds and we do not are misses, which is the safe direction.
    //
    // The count is a ratchet and may only go down.
    constexpr size_t k_accountedFindings = 7;
    CHECK(result.Total() <= k_accountedFindings);
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

    TEST_CASE("A read inside a condition is checked")
    {
        CHECK(HasUninitializedRead(AnalyzeCode(
            "void main() { int x; if (x > 0) { } }\n")));

        CHECK(HasUninitializedRead(AnalyzeCode(
            "void main() { int x; while (x > 0) { break; } }\n")));

        CHECK(HasUninitializedRead(AnalyzeCode(
            "void main() { int x; switch (x) { case 1: break; } }\n")));

        CHECK(HasUninitializedRead(AnalyzeCode(
            "void main() { int x; do { break; } while (x > 0); }\n")));
    }

    TEST_CASE("An out-argument written in a condition counts for the body after it")
    {
        // The corpus shape: `if (dict.get(key, value) && value != 0) { use(value); }`. The callee
        // is invisible, so whether that parameter is `&out` cannot be established - and `&out` is
        // what it usually is, so the variable is treated as possibly written rather than read.
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(
            "void Use(int v) { }\n"
            "void main() { UnknownDict d; int v; if (d.get('k', v) && v != 0) { Use(v); } }\n")));

        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(
            "void Use(int v) { }\n"
            "void main() { int v; if (HostGet('k', v)) { Use(v); } }\n")));
    }

    TEST_CASE("A for loop header initialises its own counter")
    {
        // `for (i = 0; ...)` assigns a variable declared earlier. The header is where `i` becomes
        // initialised, and it was being reported as reading it uninitialised.
        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(
            "void Use(int v) { }\n"
            "void main() { int i; for (i = 0; i < 3; i++) { Use(i); } }\n")));

        CHECK_FALSE(HasUninitializedRead(AnalyzeCode(
            "void Use(int v) { }\n"
            "void main() { for (int i = 0; i < 3; i++) { Use(i); } }\n")));
    }
}
