#include <doctest/doctest.h>
#include "helpers/CorpusDirectory.h"
#include "helpers/TestUtils.h"

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

TEST_CASE("CallChecker - Typedef alias matches underlying parameter type")
{
    const std::string code =
        "typedef uint EntityId;\n"
        "void SetId(EntityId id) { }\n"
        "void main()\n"
        "{\n"
        "    uint val = 10;\n"
        "    SetId(val);\n"
        "}\n";

    auto diags = AnalyzeCallSnippet(code);
    CHECK_FALSE(HasCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diags, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - Deeply nested namespace class and function calls resolve cleanly")
{
    const std::string code =
        "namespace Engine\n"
        "{\n"
        "    namespace Graphics\n"
        "    {\n"
        "        class Texture { }\n"
        "        void Bind(Texture@ t) { }\n"
        "    }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Engine::Graphics::Texture tex;\n"
        "    Engine::Graphics::Bind(tex);\n"
        "}\n";

    auto diags = AnalyzeCallSnippet(code);
    CHECK_FALSE(HasCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diags, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - Predefined engine stubs (.as.predefined) integrate with script calls")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    const std::string engineCode =
        "class Vector3 { Vector3(float x, float y, float z) { } }\n"
        "void SpawnEntity(const Vector3 &in pos, string name) { }\n";
    collector.CollectSymbols("file:///engine.as.predefined", engineCode, parser, table);

    const std::string validScript =
        "void main()\n"
        "{\n"
        "    Vector3 pos(1.0f, 2.0f, 3.0f);\n"
        "    SpawnEntity(pos, \"Player\");\n"
        "}\n";
    collector.CollectSymbols("file:///script.as", validScript, parser, table);

    SemanticAnalysisRequest request{ table, "file:///script.as", ".as.predefined", &i18n };
    request.scopeRoot = scopes.CollectScopes(validScript, parser);
    request.sourceCode = validScript;
    request.tree = parser.Parse(validScript);

    SemanticAnalyzer analyzer(nullptr);
    auto validDiags = analyzer.Analyze(request);
    if (request.tree) { ts_tree_delete(const_cast<TSTree *>(request.tree)); }

    CHECK_FALSE(HasCode(validDiags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(validDiags, "as-err-no-implicit-conversion"));

    const std::string invalidScript =
        "void main()\n"
        "{\n"
        "    SpawnEntity(\"bad_pos\", \"Player\");\n"
        "}\n";

    SymbolTable invalidTable;
    collector.CollectSymbols("file:///engine.as.predefined", engineCode, parser, invalidTable);
    collector.CollectSymbols("file:///script_invalid.as", invalidScript, parser, invalidTable);

    SemanticAnalysisRequest invRequest{ invalidTable, "file:///script_invalid.as", ".as.predefined", &i18n };
    invRequest.scopeRoot = scopes.CollectScopes(invalidScript, parser);
    invRequest.sourceCode = invalidScript;
    invRequest.tree = parser.Parse(invalidScript);

    auto invDiags = analyzer.Analyze(invRequest);
    if (invRequest.tree) { ts_tree_delete(const_cast<TSTree *>(invRequest.tree)); }

    bool hasTypeError = HasCode(invDiags, "as-err-no-implicit-conversion") ||
                        HasCode(invDiags, "as-err-call-no-matching-signature");
    CHECK(hasTypeError);
}



// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Call Argument Corpus Audit*"`)
//
// Counts all three call diagnostics, not just the argument count. It counted one of them until a
// lambda argument became judgeable and the other two acquired a new emit site with no measurement
// behind it - and widening it immediately found 102 findings that had been firing on real code,
// uncounted, since long before any of this. See the ratchet at the bottom.
// =====================================================================================

TEST_CASE("CallChecker - Call Argument Corpus Audit" * doctest::skip(true))
{
    if (!angel_lsp::test::CorpusIsAvailable())
    {
        MESSAGE(angel_lsp::test::CorpusMissingMessage());
        return;
    }

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return code == "as-err-call-argument-count" ||
               code == "as-err-call-no-matching-signature" ||
               code == "as-err-call-ambiguous";
    });

    MESSAGE("Call-argument corpus audit: files=" << result.filesAnalysed
            << " totalFlagged=" << result.Total()
            << " seconds=" << result.seconds);

    for (const auto &hit : result.hits)
    {
        MESSAGE("  " << hit.fileName << ":" << hit.line << " [" << hit.code << "] " << hit.message);
    }

    CHECK(result.filesAnalysed > 0);

    // 103 findings, and the breakdown is the point:
    //
    //   1  as-err-call-argument-count         a genuine bug in the corpus
    //  75  as-err-call-ambiguous              NOT YET DIAGNOSED
    //   4  as-err-call-no-matching-signature  not yet triaged
    //  23  the same findings again, from the second copy of the same JSON library
    //
    // The single finding is real: `Logger::Log` is declared once, taking one string, and
    // AngelScripts_CTJALoader.as:30 passes three - a format string and two values, as though an
    // overload existed for it. The same file calls it correctly with one argument eleven lines
    // later.
    //
    // THE 75 ARE NOT DIAGNOSED, and this comment used to claim they were. The claim was that
    // `bool` being wrongly listed as convertible in OverloadResolver made `Get(bool&out, bool)`
    // and `Get(int&out, bool)` tie on an int argument. `bool` WAS wrongly listed - all forty
    // combinations of {bool -> T, T -> bool} x {argument, initializer} over the ten numeric types
    // are rejected by the compiler, and the tables now say so - but correcting it did not move
    // this number at all. The hypothesis was wrong, and the measurement is what says so.
    //
    // What is known now: the mutable-reference path in ScoreArgumentMatch already refused an `int`
    // against a `bool&out` before any of that, so the `&out` overload set was never the tie; and
    // the duplicate-declaration guard in ResolveBestOverload (HasSameSignature) already handles
    // the same function arriving twice, so two copies of one library are not it either. That
    // leaves a genuine tie between two DIFFERENT signatures - `opAssign(json@)` against
    // `opAssign(const float)` for a `json@` argument, say - and the shape has not been reproduced
    // outside the corpus: three hand-written versions of it, including the namespaced spelling,
    // all resolve cleanly.
    //
    // So it is recorded rather than explained, which is the honest state. See PARITY-BACKLOG.md.
    // The number is a ratchet and may only go down.
    //
    // Seven others were reported by the first version of this rule, and every one was a false
    // positive fixed at its source rather than suppressed: a mixin body GetEnclosingContainers did
    // not recognise as a class, a funcdef construction read as a call, globals matched across two
    // plugins that never include one another, and unqualified names that could have been
    // constructions of engine-registered types. See FindFreeCandidates for what each one cost.
    CHECK(result.Total() <= 103);

    // The argument-count finding specifically, named rather than counted: it is the one this
    // audit was written for, and widening the audit must not lose it among the other two codes.
    CHECK(std::any_of(result.hits.begin(), result.hits.end(), [](const auto &hit)
    {
        return hit.code == "as-err-call-argument-count" &&
               hit.fileName == "AngelScripts_CTJALoader.as";
    }));
}

// =====================================================================================
// Constructors are not callable on an instance, and the array/template forms that ARE valid.
//
// Every expectation below was taken from the real AngelScript compiler via asharness, not from
// reading the spec:
//
//   myInt.array()                          -> error, "No matching symbol 'array'"
//   t.Thing()                              -> error, "No matching symbol 'Thing'"
//   s.string()                             -> error, "No matching symbol 'string'"
//   array<int> a(1);  array<int> a(3, 7);  -> valid
//   array<int> a = {1,2,3};                -> valid
//   array<array<int>> g = {{1,2},{3,4}};   -> valid
//   array<array<array<array<int>>>> x;     -> valid  (the >>>> really does close four templates)
//   int[][][][][][] y;                     -> valid
//
// The constructor case resolved silently before this rule existed, and for a specific reason: a
// constructor is stored as `Thing::Thing`, which is exactly the key a method lookup builds, so the
// lookup succeeded.
// =====================================================================================

TEST_CASE("CallChecker - A constructor cannot be called as a method")
{
    const char *script = R"(
        class Thing
        {
            Thing() {}
            int Value() { return 1; }
        }

        void main()
        {
            Thing t;
            t.Thing();
        }
    )";

    auto doc = angel_lsp::test::CreateTestDocument("file:///ctor_as_method.as", script);
    REQUIRE(static_cast<bool>(doc));

    bool reported = false;
    for (const auto &d : doc->GetDiagnostics())
    {
        if (d.code == "as-err-constructor-not-callable")
            reported = true;
    }
    CHECK(reported);
}

TEST_CASE("CallChecker - Ordinary methods on the same class are still fine")
{
    // The guard against the rule above becoming over-eager: only the class's own name is a
    // constructor, and every other member must keep resolving.
    const char *script = R"(
        class Thing
        {
            Thing() {}
            int Value() { return 1; }
        }

        void main()
        {
            Thing t;
            t.Value();
        }
    )";

    auto doc = angel_lsp::test::CreateTestDocument("file:///method_ok.as", script);
    REQUIRE(static_cast<bool>(doc));
    CHECK(doc->GetDiagnostics().empty());
}

TEST_CASE("CallChecker - Valid array and nested-template declarations produce no diagnostics")
{
    const char *script = R"(
        void main()
        {
            array<int> sized(1);
            array<int> filled(3, 7);
            array<int> listed = {1, 2, 3};
            array<array<int>> grid = {{1, 2}, {3, 4}};
            array<array<array<array<int>>>> deep;
            int[][][][][][] brackets;
        }
    )";

    auto doc = angel_lsp::test::CreateTestDocument("file:///array_forms.as", script);
    REQUIRE(static_cast<bool>(doc));

    for (const auto &d : doc->GetDiagnostics())
    {
        CAPTURE(d.code);
        CAPTURE(d.message);
        CHECK(d.severity != angel_lsp::analysis::DiagnosticSeverity::Error);
    }
}

// =====================================================================================
// Which argument is at fault.
//
// When no overload can take the call, the useful message names the argument rather than the call.
// That is only honest when every candidate fails at the same position: if one rejects argument 0
// and another argument 1, there is no single offending argument and the generic message is right.
// =====================================================================================

TEST_CASE("CallChecker - Blames the argument every overload rejects")
{
    const std::string code =
        "class Thing {}\n"
        "void f(int a) {}\n"
        "void f(float a) {}\n"
        "void main() { Thing t; f(t); }\n";

    const auto diagnostics = AnalyzeCallSnippet(code);
    CHECK(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - Stays generic when the overloads disagree about which argument is wrong")
{
    // `g(int, A)` gets past argument 0 and fails at 1; `g(A, A)` fails at 0. No single position
    // explains the call, and the real compiler agrees - it answers this one with
    // "No matching signatures to 'g(const int, B&)'" rather than blaming an argument.
    const std::string code =
        "class A {}\n"
        "class B {}\n"
        "void g(int a, A b) {}\n"
        "void g(A a, A b) {}\n"
        "void main() { B x; g(1, x); }\n";

    const auto diagnostics = AnalyzeCallSnippet(code);
    CHECK(HasCode(diagnostics, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

// =====================================================================================
// Unqualified calls inside a namespace.
//
// The collector keys a namespaced function under its qualified name alone - `TEST::my_test_func`,
// never `my_test_func` - and this pass used to probe only the unqualified spelling. Inside a
// namespace it therefore found no candidate for any call and checked nothing, while the identical
// call at file scope was checked. AngelScript's own answer to all four cases below:
//
//     ERROR (10, 9): No matching signatures to 'my_test_func(int)'
//
// See tests/parity/doc_r12 and doc_r14.
// =====================================================================================

TEST_CASE("CallChecker - an argument type is checked inside a namespace")
{
    const auto diagnostics = AnalyzeCallSnippet(
        "namespace TEST {\n"
        "    void main_func() { int id = 1; my_test_func(id); }\n"
        "    void my_test_func(string test_id) { }\n"
        "}\n");

    CHECK(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - passing an int where an enum is expected, inside a namespace")
{
    // The case as reported: the parameter's type is the enum and the argument is a plain int.
    const auto diagnostics = AnalyzeCallSnippet(
        "enum MyEnum { MY_TEST_ID_1, MY_TEST_ID_2 };\n"
        "namespace TEST {\n"
        "    void main_func() { int id = 1; my_test_func(id); }\n"
        "    void my_test_func(MyEnum test_id) { }\n"
        "}\n");

    CHECK(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - a correct call inside a namespace stays silent")
{
    const auto diagnostics = AnalyzeCallSnippet(
        "enum MyEnum { MY_TEST_ID_1, MY_TEST_ID_2 };\n"
        "namespace TEST {\n"
        "    void main_func() { my_test_func(MyEnum::MY_TEST_ID_1); }\n"
        "    void my_test_func(MyEnum test_id) { }\n"
        "}\n");

    CHECK(diagnostics.empty());
}

TEST_CASE("CallChecker - a nested namespace reaches its parent's functions")
{
    // Innermost first, then each enclosing scope, then global - which is the order AngelScript
    // looks in. `Outer::helper` has to be reachable from inside `Outer::Inner`.
    const auto diagnostics = AnalyzeCallSnippet(
        "namespace Outer {\n"
        "    void helper(string s) { }\n"
        "    namespace Inner {\n"
        "        void use() { int id = 1; helper(id); }\n"
        "    }\n"
        "}\n");

    CHECK(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - a call through a using-directive is judged")
{
    // This test used to assert the opposite, under the heading "left unjudged". The compiler
    // disagrees - tests/parity/doc_r16_using_ns_arg_type.as:
    //
    //     ERROR (3, 24): No matching signatures to 'f(int)'
    //
    // A using-directive puts the name in reach, so a call through one is an ordinary call.
    const auto diagnostics = AnalyzeCallSnippet(
        "namespace A { void f(string s) { } }\n"
        "using namespace A;\n"
        "void g() { int id = 1; f(id); }\n");

    CHECK(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - two using-directives contribute at once")
{
    // A directive does not shadow and does not stop the search, so both namespaces are in the
    // candidate set and overload resolution picks B::f(int). doc_p16_using_ns_overloads_merge.as.
    const auto diagnostics = AnalyzeCallSnippet(
        "namespace A { void f(string s) { } }\n"
        "namespace B { void f(int i) { } }\n"
        "using namespace A;\n"
        "using namespace B;\n"
        "void g() { f(1); }\n");

    CHECK(diagnostics.empty());
}

TEST_CASE("CallChecker - a lexical scope shadows a using-directive")
{
    // The global scope declares `f`, so the search stops there and never reaches the directive.
    // doc_p17_global_beats_using.as, which the compiler accepts.
    const auto diagnostics = AnalyzeCallSnippet(
        "namespace A { void f(string s) { } }\n"
        "void f(int i) { }\n"
        "using namespace A;\n"
        "void g() { f(1); }\n");

    CHECK(diagnostics.empty());
}

TEST_CASE("CallChecker - an inner namespace shadows the global scope")
{
    // doc_r17_inner_ns_shadows.as:
    //     ERROR (2, 46): No matching signatures to 'f(const int)'
    // `N::f(string)` hides the global `f(int)`, so the call does not fall through to it. This is
    // why candidate collection stops at the first scope that declares the name.
    const auto diagnostics = AnalyzeCallSnippet(
        "void f(int i) { }\n"
        "namespace N { void f(string s) { } void g() { f(1); } }\n");

    CHECK(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

// =====================================================================================
// A typedef inside a template argument.
//
// AngelScript's typedef names a primitive, and the name is the type - inside a template argument
// as much as anywhere else. The compiler accepts all four directions:
//
//   typedef uint8 byte;
//   void Take(array<uint8> d);  array<byte> b;  Take(b);   accepted
//   void Take(array<byte> d);   array<uint8> b; Take(b);   accepted
//
// tests/parity/doc_p19_typedef_template_argument.as. UnwrapTypedef ran on the outer name only, so
// `array<byte>` and `array<uint8>` were two different types to overload resolution and a legal
// call was reported - a false positive, which is the one failure mode this project does not accept.
// =====================================================================================

TEST_CASE("CallChecker - A typedef inside a template argument is the type it names")
{
    const std::string code =
        "class array<T> { uint length() const; }\n"
        "typedef uint8 byte;\n"
        "void Take(array<uint8> data) {}\n"
        "void main() { array<byte> b; Take(b); }\n";

    const auto diagnostics = AnalyzeCallSnippet(code);

    CHECK_FALSE(HasCode(diagnostics, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - A typedef in the parameter's template argument is the same type too")
{
    const std::string code =
        "class array<T> { uint length() const; }\n"
        "typedef uint8 byte;\n"
        "void Take(array<byte> data) {}\n"
        "void main() { array<uint8> b; Take(b); }\n";

    const auto diagnostics = AnalyzeCallSnippet(code);

    CHECK_FALSE(HasCode(diagnostics, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - A typedef in a nested template argument is unwrapped as well")
{
    const std::string code =
        "class array<T> { uint length() const; }\n"
        "typedef uint8 byte;\n"
        "void Take(array<array<uint8>> data) {}\n"
        "void main() { array<array<byte>> b; Take(b); }\n";

    const auto diagnostics = AnalyzeCallSnippet(code);

    CHECK_FALSE(HasCode(diagnostics, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - Unwrapping a template argument does not make unrelated types match")
{
    // The guard for the fix: unwrapping must not turn `array<byte>` into something that satisfies
    // `array<string>`. The compiler answers this one "No matching signatures to 'Take(uint8[]&)'",
    // and this analyzer names the argument rather than the call, because with a single candidate
    // there is one parameter to blame.
    const std::string code =
        "class array<T> { uint length() const; }\n"
        "typedef uint8 byte;\n"
        "void Take(array<string> data) {}\n"
        "void main() { array<byte> b; Take(b); }\n";

    const auto diagnostics = AnalyzeCallSnippet(code);

    CHECK(HasCode(diagnostics, "as-err-no-implicit-conversion"));
}

// =====================================================================================
// Bracket-declared arrays.
//
// `int[]` and `array<int>` are the same type - tests/parity/doc_p09_bracket_array_members.as -
// but only the template spelling reached the member-resolution path. The bracket one cleaned to
// `int`, which has no hierarchy, so the visibility guard sent every call on it away unchecked:
// no hover, no completion, no argument counting. The two spellings must answer identically.
// =====================================================================================

TEST_CASE("CallChecker - A method call on a bracket-declared array is judged")
{
    const std::string bracket =
        "class array<T> { uint length() const; void insertLast(const T&in value); }\n"
        "void main() { int[] a; a.insertLast(); }\n";

    CHECK(HasCode(AnalyzeCallSnippet(bracket), "as-err-call-argument-count"));
}

TEST_CASE("CallChecker - Both array spellings answer a correct call the same way")
{
    const std::string bracket =
        "class array<T> { uint length() const; void insertLast(const T&in value); }\n"
        "void main() { int[] a; a.insertLast(1); uint n = a.length(); }\n";

    const std::string templated =
        "class array<T> { uint length() const; void insertLast(const T&in value); }\n"
        "void main() { array<int> a; a.insertLast(1); uint n = a.length(); }\n";

    std::vector<std::string> bracketCodes;
    for (const auto &d : AnalyzeCallSnippet(bracket)) bracketCodes.push_back(d.code);
    std::vector<std::string> templatedCodes;
    for (const auto &d : AnalyzeCallSnippet(templated)) templatedCodes.push_back(d.code);

    // Identical, which is the point: the two spellings are one type and must be judged as one.
    // Neither list is empty - the minimal `array<T>` stub here has bodyless methods and an unused
    // local, which both snippets share - and that is exactly why this compares them to each other
    // rather than to nothing.
    CHECK(bracketCodes == templatedCodes);
    CHECK_FALSE(HasCode(AnalyzeCallSnippet(bracket), "as-err-call-argument-count"));
}

TEST_CASE("CallChecker - A nested bracket array resolves to the outer container")
{
    // `int[][]` is `array<array<int>>`, so the element canonicalises before it is wrapped.
    const std::string code =
        "class array<T> { uint length() const; void insertLast(const T&in value); }\n"
        "void main() { int[][] grid; grid.insertLast(); }\n";

    CHECK(HasCode(AnalyzeCallSnippet(code), "as-err-call-argument-count"));
}

// =====================================================================================
// A lambda handed to a function whose parameter is a funcdef handle.
//
// A lambda resolves to no type, so `allArgsResolved` was false and the whole overload check was
// skipped for any call taking one. Every lambda in the 1,061-file corpus is a call argument, so
// that silence covered all of them.
//
// Each expectation below is a recording of what `angelscript_oracle` answered.
// =====================================================================================

namespace
{
    bool RejectsLambdaCall(const std::string &code)
    {
        return HasCode(AnalyzeCallSnippet(code), "as-err-call-no-matching-signature");
    }

    bool AmbiguousLambdaCall(const std::string &code)
    {
        return HasCode(AnalyzeCallSnippet(code), "as-err-call-ambiguous");
    }

    bool SilentOnLambdaCall(const std::string &code)
    {
        const auto diagnostics = AnalyzeCallSnippet(code);
        return !HasCode(diagnostics, "as-err-call-no-matching-signature") &&
               !HasCode(diagnostics, "as-err-call-ambiguous") &&
               !HasCode(diagnostics, "as-err-call-argument-count");
    }
}

TEST_CASE("CallChecker - A lambda argument is judged against the funcdef parameter")
{
    // Oracle: "No matching signatures to 'Take(<auto> lambda())'".
    CHECK(RejectsLambdaCall(
        "funcdef void CB(int);\n"
        "void Take(CB@ c) {}\n"
        "void main() { Take(function() { }); }\n"));

    CHECK(SilentOnLambdaCall(
        "funcdef void CB(int);\n"
        "void Take(CB@ c) {}\n"
        "void main() { Take(function(int a) { }); }\n"));

    // Untyped parameters take their type from the funcdef, so this one is accepted.
    CHECK(SilentOnLambdaCall(
        "funcdef void CB(int);\n"
        "void Take(CB@ c) {}\n"
        "void main() { Take(function(a) { }); }\n"));

    // Oracle: "No matching signatures to 'C::Take(<auto> lambda())'" - a method is judged too.
    CHECK(RejectsLambdaCall(
        "funcdef void CB(int);\n"
        "class C { void Take(CB@ c) {} }\n"
        "void main() { C c; c.Take(function() { }); }\n"));

    // Not only in first position.
    CHECK(RejectsLambdaCall(
        "funcdef void CB(int);\n"
        "void Take(int n, CB@ c) {}\n"
        "void main() { Take(1, function() { }); }\n"));

    CHECK(SilentOnLambdaCall(
        "funcdef void CB(int);\n"
        "void Take(int n, CB@ c) {}\n"
        "void main() { Take(1, function(int a) { }); }\n"));
}

TEST_CASE("CallChecker - A lambda argument written wrong is rejected the same way as an assignment")
{
    // The decorations and the written type carry over from the assignment rule; the oracle
    // rejects each of these with "No matching signatures".
    CHECK(RejectsLambdaCall(
        "funcdef void CB(int);\n"
        "void Take(CB@ c) {}\n"
        "void main() { Take(function(uint a) { }); }\n"));

    CHECK(RejectsLambdaCall(
        "funcdef void CB(const string &in);\n"
        "void Take(CB@ c) {}\n"
        "void main() { Take(function(string s) { }); }\n"));

    CHECK(SilentOnLambdaCall(
        "funcdef void CB(const string &in);\n"
        "void Take(CB@ c) {}\n"
        "void main() { Take(function(const string &in s) { }); }\n"));
}

TEST_CASE("CallChecker - Overloads: one match passes, none match reports, several are ambiguous")
{
    // Oracle, in order: accepted; "No matching signatures to 'T(<auto> lambda(int, int))'";
    // "Multiple matching signatures to 'T(<auto> lambda(<auto>))'" - because an untyped lambda of
    // arity one satisfies both funcdefs and nothing chooses between them.
    const std::string overloads =
        "funcdef void A(int);\n"
        "funcdef void B(string);\n"
        "void T(A@ c) {}\n"
        "void T(B@ c) {}\n";

    CHECK(SilentOnLambdaCall(overloads + "void main() { T(function(int a) { }); }\n"));
    CHECK(RejectsLambdaCall(overloads + "void main() { T(function(int a, int b) { }); }\n"));
    CHECK(AmbiguousLambdaCall(overloads + "void main() { T(function(a) { }); }\n"));
}

TEST_CASE("CallChecker - A callback the workspace cannot see is not judged at all")
{
    // The usual shape in real code: `Hooks.RegisterHook(..., @MapChangeHook(function(...)))`,
    // where MapChangeHook is registered by the host in C++ and declared in no script. With the
    // funcdef invisible the parameter's signature is unknown, so there is nothing to compare
    // against - and silence is the whole visibility policy, not a gap in this rule.
    CHECK(SilentOnLambdaCall(
        "void Take(UnknownCallback@ c) {}\n"
        "void main() { Take(function() { }); }\n"));

    // Even when one overload IS visible: which one the call meant is undecided.
    CHECK(SilentOnLambdaCall(
        "funcdef void CB(int);\n"
        "void Take(CB@ c) {}\n"
        "void Take(UnknownCallback@ c) {}\n"
        "void main() { Take(function() { }); }\n"));
}

TEST_CASE("CallChecker - The same funcdef reached twice is not an ambiguity")
{
    // Not a compiler case - the snippet below is a duplicate definition the compiler rejects
    // outright. It stands in for the shape the corpus really produces: twenty projects flattened
    // into one directory, so a function declared once per project arrives in the shared symbol
    // table as several identical candidates. Two of them naming the SAME funcdef is that, not the
    // ambiguity the compiler means, so the ambiguity check compares the funcdefs rather than
    // counting the candidates.
    CHECK(SilentOnLambdaCall(
        "funcdef void CB(int);\n"
        "void Take(CB@ c) {}\n"
        "void Take(CB@ c) {}\n"
        "void main() { Take(function(int a) { }); }\n"));
}

namespace
{
    /** @brief True when a conversion diagnostic names both types in its message. */
    bool HasBoolConversion(const std::vector<Diagnostic> &diagnostics,
                           const std::string &from,
                           const std::string &to)
    {
        const std::string quotedFrom = "'" + from + "'";
        const std::string quotedTo = "'" + to + "'";
        return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic &diag)
        {
            return diag.code == "as-err-no-implicit-conversion" &&
                   diag.message.find(quotedFrom) != std::string::npos &&
                   diag.message.find(quotedTo) != std::string::npos;
        });
    }
}

TEST_CASE("CallChecker - Two overloads differing only in bool versus int are not ambiguous")
{
    // The 75-finding false positive, in miniature. `bool` was listed as convertible to and from
    // the numeric types, so an int argument scored against BOTH `bool&out` and `int&out` and tied.
    // Oracle: the call is accepted, unambiguously, because only one of those parameters can take
    // an int at all - and with only the bool overload present it is "No matching signatures to
    // 'J::Get(int, bool)'".
    const std::string set =
        "class J {\n"
        "  bool Get(bool &out v, bool s = true) const { v = true; return s; }\n"
        "  bool Get(int &out v, bool s = true) const { v = 1; return s; }\n"
        "}\n";

    const auto diagnostics = AnalyzeCallSnippet(
        set + "void main() { J j; int n; bool st = true; j.Get(n, st); }\n");
    CHECK_FALSE(HasCode(diagnostics, "as-err-call-ambiguous"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-call-no-matching-signature"));

    // Only the bool overload: now there is genuinely nothing to call. Reported by naming the
    // offending argument rather than with the generic message - with one candidate, "which
    // argument is at fault" is not in doubt, and this checker says so on purpose.
    CHECK(HasCode(AnalyzeCallSnippet(
        "class J { bool Get(bool &out v, bool s = true) const { v = true; return s; } }\n"
        "void main() { J j; int n; bool st = true; j.Get(n, st); }\n"),
        "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - A bool argument does not satisfy a numeric parameter, or the reverse")
{
    // Oracle: "No matching signatures to 'f(bool)'" and "No matching signatures to 'f(int)'" -
    // said here as the offending argument, which is the same refusal put more precisely.
    CHECK(HasBoolConversion(AnalyzeCallSnippet(
        "void f(int v) {}\n"
        "void main() { bool b = true; f(b); }\n"), "bool", "int"));

    CHECK(HasBoolConversion(AnalyzeCallSnippet(
        "void f(bool v) {}\n"
        "void main() { int n = 1; f(n); }\n"), "int", "bool"));

    // The matching call stays silent, which is the half that guards against over-correcting.
    CHECK_FALSE(HasCode(AnalyzeCallSnippet(
        "void f(bool v) {}\n"
        "void main() { bool b = true; f(b); }\n"), "as-err-no-implicit-conversion"));
}

TEST_CASE("CallChecker - A method's &out parameter initialises its argument")
{
    // `int n; obj.Get(n);` is how you initialise `n`, and it was reported as using it
    // uninitialised. The out-parameter rule existed and worked for free functions; for a METHOD
    // it resolved the receiver's type in the ROOT scope, where a local never lives, so no
    // candidate was found and the parameter's `&out` was never seen. Found by doc_p28, which the
    // real compiler accepts without even a warning.
    const auto flagged = [](const std::string &code)
    {
        return HasCode(AnalyzeCallSnippet(code), "as-err-uninitialized-variable-read");
    };

    CHECK_FALSE(flagged(
        "class R { bool Get(int &out v) const { v = 1; return true; } }\n"
        "void main() { R r; int n; r.Get(n); }\n"));

    CHECK_FALSE(flagged(
        "class R { bool Get(int &out v, bool s = true) const { v = 1; return s; } }\n"
        "void main() { R r; int n; bool st = true; r.Get(n, st); }\n"));

    CHECK_FALSE(flagged(
        "class R { bool Get(bool &out v, bool s = true) const { v = true; return s; }\n"
        "          bool Get(int &out v, bool s = true) const { v = 1; return s; } }\n"
        "void main() { R r; int n; bool st = true; r.Get(n, st); }\n"));

    // The free-function form, which always worked and must keep working.
    CHECK_FALSE(flagged(
        "bool Get(int &out v, bool s = true) { v = 1; return s; }\n"
        "void main() { int n; bool st = true; Get(n, st); }\n"));

    // And the rule still fires where the parameter is NOT an out - which is the half that shows
    // the fix widened the lookup rather than switching the check off.
    CHECK(flagged(
        "class R { bool Get(int v) const { return v > 0; } }\n"
        "void main() { R r; int n; r.Get(n); }\n"));
}


TEST_CASE("CallChecker - A bracket-array size is not an element conversion")
{
    // `T[] name(size)` sizes an array. The bracket spelling reduces to its ELEMENT type in this
    // pass, so it used to be read as constructing one `T` from the size - silently, because
    // `int -> int` and `int -> float` score fine. `bool[] flags(33);` is the same declaration and
    // the compiler accepts it; it only became visible once bool stopped converting from int.
    const auto flagged = [](const std::string &code)
    {
        return HasCode(AnalyzeCallSnippet(code), "as-err-no-implicit-conversion") ||
               HasCode(AnalyzeCallSnippet(code), "as-err-no-matching-constructor");
    };

    CHECK_FALSE(flagged("bool[] flags(33);\nvoid main() { }\n"));
    CHECK_FALSE(flagged("int[] counts(33);\nvoid main() { }\n"));
    CHECK_FALSE(flagged("float[] weights(32 + 1);\nvoid main() { }\n"));
    CHECK_FALSE(flagged("void main() { bool[] flags(33); }\n"));

    // The angle spelling always took the class path and must keep taking it.
    CHECK_FALSE(flagged("array<bool> flags(33);\nvoid main() { }\n"));

    // A real primitive construction is still judged: `bool b(33)` is not an array.
    CHECK(flagged("void main() { bool b(33); }\n"));
}
