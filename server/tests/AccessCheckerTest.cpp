#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "config/ServerConfig.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// Access control on member use.
//
// Every expectation here was compiled with a real AngelScript build first. Two of them are not
// what the words "private" and "protected" alone suggest: private is per class rather than per
// instance, and protected is reachable only through an object of the accessing class's own type.
// =====================================================================================

namespace
{
    std::vector<Diagnostic> AnalyzeAccessSnippet(const std::string &code,
                                                 const std::string &fileUri = "file:///access.as",
                                                 const angel_lsp::config::EngineProperties *engine = nullptr)
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static angel_lsp::i18n::I18n i18n;

        collector.CollectSymbols(fileUri, code, parser, table);

        SemanticAnalysisRequest request{ table, fileUri, ".as.predefined", &i18n };
        request.engineProperties = engine;
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

    bool HasNoAccessFinding(const std::vector<Diagnostic> &diagnostics)
    {
        return !HasCode(diagnostics, "as-err-private-member-access") &&
               !HasCode(diagnostics, "as-err-protected-member-access");
    }
}

// =====================================================================================
// private
// =====================================================================================

TEST_CASE("AccessChecker - Reports a private field written from outside its class")
{
    const std::string code =
        "class MyClass\n"
        "{\n"
        "    private float f;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    MyClass myClass;\n"
        "    myClass.f = 3.0f;\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-private-member-access"));
}

TEST_CASE("AccessChecker - Reports a private method called from outside its class")
{
    const std::string code =
        "class MyClass\n"
        "{\n"
        "    private void Hidden() { }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    MyClass myClass;\n"
        "    myClass.Hidden();\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-private-member-access"));
}

TEST_CASE("AccessChecker - A public field beside a private one is left alone")
{
    const std::string code =
        "class MyClass\n"
        "{\n"
        "    private float f;\n"
        "    float visible;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    MyClass myClass;\n"
        "    myClass.visible = 3.0f;\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - private is per class, not per instance")
{
    // Inside MyClass, a second MyClass is fully open. This is the case an instance-based reading of
    // "private" would report, and the engine does not.
    const std::string code =
        "class MyClass\n"
        "{\n"
        "    private float f;\n"
        "    void Copy(MyClass@ other) { other.f = f; }\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - this reaches the class's own private member")
{
    const std::string code =
        "class MyClass\n"
        "{\n"
        "    private float f;\n"
        "    void Reset() { this.f = 0.0f; }\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - A derived class cannot reach a private base member")
{
    const std::string code =
        "class Base\n"
        "{\n"
        "    private float f;\n"
        "}\n"
        "class Derived : Base\n"
        "{\n"
        "    void Touch() { Derived d; d.f = 1.0f; }\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-private-member-access"));
}

// =====================================================================================
// protected
// =====================================================================================

TEST_CASE("AccessChecker - Reports a protected member reached from outside the hierarchy")
{
    const std::string code =
        "class Base\n"
        "{\n"
        "    protected int p;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Base b;\n"
        "    b.p = 1;\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-protected-member-access"));
}

TEST_CASE("AccessChecker - A derived class reaches a protected member through its own type")
{
    const std::string code =
        "class Base\n"
        "{\n"
        "    protected int p;\n"
        "}\n"
        "class Derived : Base\n"
        "{\n"
        "    void Touch() { Derived d; d.p = 1; }\n"
        "    void Self() { this.p = 2; }\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - A derived class cannot reach a protected member through a base-typed object")
{
    // The rule that reads like a mistake and is not: Derived inherits p, but a Base object is not a
    // Derived, and the engine rejects it.
    const std::string code =
        "class Base\n"
        "{\n"
        "    protected int p;\n"
        "}\n"
        "class Derived : Base\n"
        "{\n"
        "    void Touch() { Base b; b.p = 1; }\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-protected-member-access"));
}

// =====================================================================================
// Staying silent
// =====================================================================================

TEST_CASE("AccessChecker - An unresolved base means the member could be anything")
{
    // CEngineType is registered by the host and declared nowhere here, so what Wrapper really
    // carries - and under what access - is not knowable from this document.
    const std::string code =
        "class Wrapper : CEngineType\n"
        "{\n"
        "    private int hidden;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Wrapper w;\n"
        "    w.hidden = 1;\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - An object of unknown type is not judged")
{
    const std::string code =
        "void main()\n"
        "{\n"
        "    CBasePlayer@ player = null;\n"
        "    player.pev = null;\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - A public overload keeps the name reachable")
{
    // Overloads can differ in access, and the engine picks by signature. Reporting the call because
    // one of the two is private would be wrong for every use of the public one.
    const std::string code =
        "class MyClass\n"
        "{\n"
        "    private void Fire() { }\n"
        "    void Fire(int rounds) { }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    MyClass myClass;\n"
        "    myClass.Fire(3);\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - A mixin body is not judged")
{
    // A mixin's methods are compiled into every class that includes it, so what they may reach is
    // decided somewhere else entirely.
    const std::string code =
        "class Target\n"
        "{\n"
        "    private int hidden;\n"
        "}\n"
        "mixin class Helper\n"
        "{\n"
        "    void Poke(Target@ t) { t.hidden = 1; }\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - A mixin's private member belongs to the class that includes it")
{
    // Including a mixin copies its members in, so Name is private to Hook, not to NameGetter. This
    // was the rule's only false positive over the corpus, on a line as ordinary as they come.
    const std::string code =
        "mixin class NameGetter\n"
        "{\n"
        "    private string Name;\n"
        "}\n"
        "class Hook : NameGetter\n"
        "{\n"
        "    Hook(const string &in name) { this.Name = name; }\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - A class deriving from a mixin's includer still reaches the member")
{
    const std::string code =
        "mixin class NameGetter\n"
        "{\n"
        "    private string Name;\n"
        "}\n"
        "class Base : NameGetter { }\n"
        "class Derived : Base\n"
        "{\n"
        "    void Set() { this.Name = \"x\"; }\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - A mixin's private member is still closed from outside")
{
    const std::string code =
        "mixin class NameGetter\n"
        "{\n"
        "    private string Name;\n"
        "}\n"
        "class Hook : NameGetter { }\n"
        "void main()\n"
        "{\n"
        "    Hook h;\n"
        "    h.Name = \"x\";\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-private-member-access"));
}

TEST_CASE("AccessChecker - A stub is not read as if it used the API it declares")
{
    const std::string code =
        "class MyClass\n"
        "{\n"
        "    private float f;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    MyClass myClass;\n"
        "    myClass.f = 3.0f;\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code, "file:///as.predefined")));
}

// =====================================================================================
// Expression forms the object can arrive as
//
// This pass can only judge what it can resolve, so every expression shape ResolveExpressionType
// learns is a shape access control starts covering. These are the ones that name a type rather
// than compute one - a cast writes its answer down, an index and a unary operator carry their
// operand's through - which is why they can be answered without the engine's promotion rules.
// =====================================================================================

TEST_CASE("AccessChecker - Reaches a private member through a cast")
{
    // The idiom the corpus is built on: an engine handle cast to the type that actually has the
    // members. Before the cast resolved, every one of these was invisible to this pass.
    const std::string code =
        "class Base {}\n"
        "class Derived : Base\n"
        "{\n"
        "    private int hidden;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Base@ b;\n"
        "    cast<Derived@>(b).hidden = 1;\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-private-member-access"));
}

TEST_CASE("AccessChecker - Reaches a private member through an array element")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    private int hidden;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    array<Entity@> entities;\n"
        "    entities[0].hidden = 1;\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-private-member-access"));
}

TEST_CASE("AccessChecker - Reaches a private member through a handle-of operator")
{
    // Written through the operator itself rather than through a variable holding its result, so
    // the unary branch is what has to answer and not the identifier one.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    private int hidden;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Entity e;\n"
        "    (@e).hidden = 1;\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-private-member-access"));
}

TEST_CASE("AccessChecker - A public member reached the same ways stays quiet")
{
    // The other half of every one of the cases above: resolving more expressions must widen what
    // is judged, not what is reported.
    const std::string code =
        "class Base {}\n"
        "class Derived : Base\n"
        "{\n"
        "    int open;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Base@ b;\n"
        "    array<Derived@> many;\n"
        "    cast<Derived@>(b).open = 1;\n"
        "    many[0].open = 2;\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

TEST_CASE("AccessChecker - An expression this analyzer cannot type is still never judged")
{
    // Binary and ternary expressions need the engine's promotion and operator-overload rules, so
    // they resolve to nothing and the access is left alone - the same contract every unresolved
    // object has always had.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    private int hidden;\n"
        "    Entity@ opAdd(Entity@ other) { return this; }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Entity@ a;\n"
        "    Entity@ b;\n"
        "    (a + b).hidden = 1;\n"
        "}\n";

    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code)));
}

// =====================================================================================
// asEP_PRIVATE_PROP_AS_PROTECTED
//
// The one engine option that changes this pass. With it set, a private member follows the
// protected rule instead of the private one - so what the option really moves is the boundary
// between "only my own class" and "my class and everything derived from it".
// =====================================================================================

TEST_CASE("AccessChecker - privatePropAsProtected opens a private member to a derived class")
{
    const std::string code =
        "class Base\n"
        "{\n"
        "    private int hidden;\n"
        "}\n"
        "class Derived : Base\n"
        "{\n"
        "    void Use()\n"
        "    {\n"
        "        Derived d;\n"
        "        d.hidden = 1;\n"
        "    }\n"
        "}\n";

    CHECK(HasCode(AnalyzeAccessSnippet(code), "as-err-private-member-access"));

    angel_lsp::config::EngineProperties engine;
    engine.privatePropAsProtected = true;
    CHECK(HasNoAccessFinding(AnalyzeAccessSnippet(code, "file:///access.as", &engine)));
}

TEST_CASE("AccessChecker - privatePropAsProtected does not open a private member to a stranger")
{
    // Protected is still an access rule, not the absence of one. An unrelated class gains nothing.
    const std::string code =
        "class Base\n"
        "{\n"
        "    private int hidden;\n"
        "}\n"
        "class Stranger\n"
        "{\n"
        "    void Use()\n"
        "    {\n"
        "        Base b;\n"
        "        b.hidden = 1;\n"
        "    }\n"
        "}\n";

    angel_lsp::config::EngineProperties engine;
    engine.privatePropAsProtected = true;

    // Still reported, and still reported as private - the member was written `private`, and the
    // option changes which rule decides the access rather than what the declaration says.
    CHECK(HasCode(AnalyzeAccessSnippet(code, "file:///access.as", &engine),
                  "as-err-private-member-access"));
}

TEST_CASE("AccessChecker - privatePropAsProtected leaves the protected rule where it was")
{
    // Reaching a base-typed object's protected member is an error even from a class that inherits
    // it, and no engine option in play here changes that.
    const std::string code =
        "class Base\n"
        "{\n"
        "    protected int guarded;\n"
        "}\n"
        "class Derived : Base\n"
        "{\n"
        "    void Use()\n"
        "    {\n"
        "        Base b;\n"
        "        b.guarded = 1;\n"
        "    }\n"
        "}\n";

    angel_lsp::config::EngineProperties engine;
    engine.privatePropAsProtected = true;
    CHECK(HasCode(AnalyzeAccessSnippet(code, "file:///access.as", &engine),
                  "as-err-protected-member-access"));
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Access Corpus Audit*"`)
// =====================================================================================

TEST_CASE("AccessChecker - Access Corpus Audit" * doctest::skip(true))
{
    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return code == "as-err-private-member-access" || code == "as-err-protected-member-access";
    });

    MESSAGE("Access corpus audit: files=" << result.filesAnalysed
            << " totalFlagged=" << result.Total()
            << " seconds=" << result.seconds);

    for (const auto &[code, count] : result.countByCode)
    {
        MESSAGE("  " << code << ": " << count);
    }
    for (const auto &hit : result.hits)
    {
        MESSAGE("  " << hit.fileName << ":" << hit.line << " [" << hit.code << "] " << hit.message);
    }

    CHECK(result.filesAnalysed > 0);
}
