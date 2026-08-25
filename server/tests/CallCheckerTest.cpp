#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// Argument counts at call sites.
//
// The engine answers a call it cannot match with "No matching signatures to 'Take(const int)'" -
// a failed overload lookup rather than a refusal of one signature - so this asks the same
// question: is there any visible declaration of this name that takes this many arguments?
// Every expectation was compiled against a real build first.
// =====================================================================================

namespace
{
    std::vector<Diagnostic> AnalyzeCallSnippet(const std::string &code,
                                               const std::string &fileUri = "file:///calls.as")
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

    bool HasCode(const std::vector<Diagnostic> &diagnostics, const std::string &code)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&code](const Diagnostic &diag) { return diag.code == code; });
    }

    bool Reports(const std::string &code)
    {
        return HasCode(AnalyzeCallSnippet(code), "as-err-call-argument-count");
    }
}

// =====================================================================================
// Counting
// =====================================================================================

TEST_CASE("CallChecker - Reports too few and too many arguments")
{
    CHECK(Reports("void Take(int a, int b) { }\nvoid main() { Take(1); }\n"));
    CHECK(Reports("void Take(int a, int b) { }\nvoid main() { Take(1, 2, 3); }\n"));
}

TEST_CASE("CallChecker - The right number is accepted")
{
    CHECK_FALSE(Reports("void Take(int a, int b) { }\nvoid main() { Take(1, 2); }\n"));
    CHECK_FALSE(Reports("void Take() { }\nvoid main() { Take(); }\n"));
}

TEST_CASE("CallChecker - A default value makes a parameter optional")
{
    const std::string code =
        "void Take(int a, int b = 0, int c = 1) { }\n"
        "void main()\n"
        "{\n"
        "    Take(1);\n"
        "    Take(1, 2);\n"
        "    Take(1, 2, 3);\n"
        "}\n";

    CHECK_FALSE(Reports(code));
    CHECK(Reports("void Take(int a, int b = 0) { }\nvoid main() { Take(); }\n"));
}

TEST_CASE("CallChecker - An overload that fits closes the question")
{
    // The engine's message names a failed lookup over the whole set, so one member taking this
    // many arguments is all it needs.
    const std::string code =
        "void Take(int a) { }\n"
        "void Take(int a, int b) { }\n"
        "void main() { Take(1); Take(1, 2); }\n";

    CHECK_FALSE(Reports(code));
    CHECK(Reports("void Take(int a) { }\nvoid Take(int a, int b) { }\nvoid main() { Take(1, 2, 3); }\n"));
}

TEST_CASE("CallChecker - A named argument counts once, not twice")
{
    // `Take(a: 1, b: 2)` contributes both its names and its values as named children of the
    // argument list, so counting those would see four arguments where there are two.
    CHECK_FALSE(Reports("void Take(int a, int b) { }\nvoid main() { Take(a: 1, b: 2); }\n"));
}

TEST_CASE("CallChecker - A void argument is an argument")
{
    // AngelScript's spelling of "discard this &out", and it fills a parameter slot.
    CHECK_FALSE(Reports("void Take(int &out v) { }\nvoid main() { Take(void); }\n"));
}

// =====================================================================================
// Methods
// =====================================================================================

TEST_CASE("CallChecker - Reports a method called with the wrong count")
{
    const std::string code =
        "class Entity { void Think(int a) { } }\n"
        "void main() { Entity e; e.Think(); }\n";

    CHECK(Reports(code));
}

TEST_CASE("CallChecker - A method inherited from a base is found")
{
    const std::string code =
        "class Base { void Think(int a) { } }\n"
        "class Derived : Base { }\n"
        "void main() { Derived d; d.Think(1); }\n";

    CHECK_FALSE(Reports(code));
}

TEST_CASE("CallChecker - A qualified call is judged wherever it was declared")
{
    // The one written form that names exactly one thing and cannot be a construction of something
    // invisible. It is also the shape of the single genuine bug the corpus audit found.
    const std::string declared =
        "namespace Logger\n"
        "{\n"
        "    void Log(string content) { }\n"
        "}\n";

    CHECK(Reports(declared + "void main() { Logger::Log('a', 'b', 'c'); }\n"));
    CHECK_FALSE(Reports(declared + "void main() { Logger::Log('a'); }\n"));
}

TEST_CASE("CallChecker - An unqualified call inside a class body is not judged")
{
    // Narrowed by the corpus audit. An unqualified name can be a construction of a type this
    // analyzer cannot see: `VoteBlocked(this.VoteBlocked)` builds a delegate from an
    // engine-registered funcdef, and the enclosing class declaring a method of that name is a
    // coincidence - but the funcdef is invisible, so the two are indistinguishable from here. Two
    // corpus files were reported on exactly that shape.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    void Helper(int a) { }\n"
        "    void Think() { Helper(); }\n"
        "}\n";

    CHECK_FALSE(Reports(code));
}

// =====================================================================================
// Silence where nothing can be established
// =====================================================================================

TEST_CASE("CallChecker - A callee with no visible declaration is never judged")
{
    // Most of the corpus is engine-registered functions, and reporting one would be reporting the
    // engine.
    CHECK_FALSE(Reports("void main() { g_EngineFuncs.ServerPrint(\"a\", \"b\", \"c\"); }\n"));
    CHECK_FALSE(Reports("void main() { SomeHostFunction(1, 2, 3); }\n"));
}

TEST_CASE("CallChecker - A method on a type this analyzer cannot see is never judged")
{
    CHECK_FALSE(Reports("void Take(CBaseEntity@ ent) { ent.TakeDamage(1); }\n"));
}

TEST_CASE("CallChecker - A base that does not resolve keeps the whole type unjudged")
{
    const std::string code =
        "class Derived : CBaseEntity { void Think(int a) { } }\n"
        "void main() { Derived d; d.Think(); }\n";

    CHECK_FALSE(Reports(code));
}

TEST_CASE("CallChecker - A global declared in another file is not a candidate")
{
    // Two Sven Co-op plugins that never include one another both declare `Stop`, and matching a
    // call in one against the other's signature reads a relationship that does not exist. The same
    // file is the one module boundary this pass can be certain of.
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    const std::string other = "void Stop(int player) { }\n";
    const std::string here = "void main() { Stop(); }\n";
    collector.CollectSymbols("file:///other.as", other, parser, table);
    collector.CollectSymbols("file:///here.as", here, parser, table);

    SemanticAnalysisRequest request{ table, "file:///here.as", ".as.predefined", &i18n };
    request.scopeRoot = scopes.CollectScopes(here, parser);
    request.sourceCode = here;
    request.tree = parser.Parse(here);

    SemanticAnalyzer analyzer(nullptr);
    const auto diagnostics = analyzer.Analyze(request);
    ts_tree_delete(const_cast<TSTree *>(request.tree));

    CHECK_FALSE(HasCode(diagnostics, "as-err-call-argument-count"));
}

TEST_CASE("CallChecker - A construction of a type is not a call to a function of that name")
{
    // `Callback(this.Handler)` builds a delegate; the enclosing scope declaring a function called
    // Callback would be a coincidence, and the parentheses mean construction either way.
    const std::string code =
        "funcdef void Callback(int a);\n"
        "class Entity\n"
        "{\n"
        "    void Handler(int a) { }\n"
        "    void Think() { Callback@ cb = Callback(this.Handler); }\n"
        "}\n";

    CHECK_FALSE(Reports(code));
}

TEST_CASE("CallChecker - A call through a handle variable is not a named function call")
{
    // `Callback@ cb; cb(1)` reaches whatever funcdef the handle names, which is not a question
    // this pass answers - and the local shadows any function of the same name in any case.
    const std::string code =
        "funcdef void Callback(int a);\n"
        "void Handler() { }\n"
        "void main()\n"
        "{\n"
        "    Callback@ Handler;\n"
        "    Handler(1);\n"
        "}\n";

    CHECK_FALSE(Reports(code));
}

TEST_CASE("CallChecker - A predefined stub is exempt")
{
    const std::string code =
        "void Take(int a, int b) { }\n"
        "void main() { Take(1); }\n";

    CHECK_FALSE(HasCode(AnalyzeCallSnippet(code, "file:///engine.as.predefined"),
                        "as-err-call-argument-count"));
}

TEST_CASE("CallChecker - Reports incompatible argument type for method call")
{
    const std::string code =
        "class AnotherClass\n"
        "{\n"
        "    void AnotherMethod(float f)\n"
        "    {\n"
        "    }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    AnotherClass myClass;\n"
        "    myClass.AnotherMethod(\"2.0f\");\n"
        "}\n";

    auto diags = AnalyzeCallSnippet(code);
    bool hasTypeError = HasCode(diags, "as-err-no-implicit-conversion") ||
                        HasCode(diags, "as-err-call-no-matching-signature");
    CHECK(hasTypeError);
}

TEST_CASE("CallChecker - Accepts compatible argument types")
{
    const std::string code =
        "class AnotherClass\n"
        "{\n"
        "    void AnotherMethod(float f)\n"
        "    {\n"
        "    }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    AnotherClass myClass;\n"
        "    myClass.AnotherMethod(2.0f);\n"
        "}\n";

    auto diags = AnalyzeCallSnippet(code);
    CHECK_FALSE(HasCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diags, "as-err-call-argument-count"));
}

TEST_CASE("CallChecker - Reports ambiguous call error when two overloads tie")
{
    const std::string code =
        "void Action(int a, double b) { }\n"
        "void Action(double a, int b) { }\n"
        "void main()\n"
        "{\n"
        "    Action(1, 2);\n"
        "}\n";

    auto diags = AnalyzeCallSnippet(code);
    CHECK(HasCode(diags, "as-err-call-ambiguous"));
}

TEST_CASE("CallChecker - Multi-level inheritance overload chooses most derived match")
{
    const std::string code =
        "class Base { }\n"
        "class Derived : Base { }\n"
        "class Leaf : Derived { }\n"
        "void Inspect(Base@ b) { }\n"
        "void Inspect(Derived@ d) { }\n"
        "void main()\n"
        "{\n"
        "    Leaf l;\n"
        "    Inspect(l);\n"
        "}\n";

    auto diags = AnalyzeCallSnippet(code);
    CHECK_FALSE(HasCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diags, "as-err-call-ambiguous"));
    CHECK_FALSE(HasCode(diags, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - User-defined constructor argument conversion resolves overload")
{
    const std::string code =
        "class Vector2\n"
        "{\n"
        "    Vector2(float x, float y = 0.0f) { }\n"
        "}\n"
        "void Move(Vector2 v) { }\n"
        "void Move(string s) { }\n"
        "void main()\n"
        "{\n"
        "    Move(10.0f);\n"
        "}\n";

    auto diags = AnalyzeCallSnippet(code);
    CHECK_FALSE(HasCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diags, "as-err-call-ambiguous"));
    CHECK_FALSE(HasCode(diags, "as-err-no-implicit-conversion"));
}



// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Call Argument Corpus Audit*"`)
// =====================================================================================

TEST_CASE("CallChecker - Call Argument Corpus Audit" * doctest::skip(true))
{
    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return code == "as-err-call-argument-count";
    });

    MESSAGE("Call-argument corpus audit: files=" << result.filesAnalysed
            << " totalFlagged=" << result.Total()
            << " seconds=" << result.seconds);

    for (const auto &hit : result.hits)
    {
        MESSAGE("  " << hit.fileName << ":" << hit.line << " [" << hit.code << "] " << hit.message);
    }

    CHECK(result.filesAnalysed > 0);

    // One finding survives triage over 1,061 files, and it is a genuine bug in the corpus:
    // `Logger::Log` is declared once, taking one string, and AngelScripts_CTJALoader.as:30 passes
    // three - a format string and two values, as though an overload existed for it. The same file
    // calls it correctly with one argument eleven lines later.
    //
    // Seven others were reported by the first version of this rule, and every one was a false
    // positive fixed at its source rather than suppressed: a mixin body GetEnclosingContainers did
    // not recognise as a class, a funcdef construction read as a call, globals matched across two
    // plugins that never include one another, and unqualified names that could have been
    // constructions of engine-registered types. See FindFreeCandidates for what each one cost.
    CHECK(result.Total() == 1);
    REQUIRE(result.hits.size() == 1);
    CHECK(result.hits[0].fileName == "AngelScripts_CTJALoader.as");
}
