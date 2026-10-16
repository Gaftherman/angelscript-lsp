#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/rules/ClassRules.h"
#include "helpers/CorpusDirectory.h"
#include "helpers/RuleCorpusAudit.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
/** @brief Runs the full analyzer over a snippet and returns its diagnostics. */
std::vector<Diagnostic> AnalyzeClassSnippet(const std::string& code, const std::string& fileUri = "file:///classes.as")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    auto diagnostics = collector.CollectSymbols({fileUri, code, &i18n}, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    request.tree = parser.Parse(code);

    SemanticAnalyzer analyzer(nullptr);
    auto semDiags = analyzer.Analyze(request);
    diagnostics.insert(diagnostics.end(), semDiags.begin(), semDiags.end());

    if (request.tree)
    {
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    }
    return diagnostics;
}

/** @brief Runs the analyzer over two files sharing a symbol table and returns diagnostics for the second file. */
std::vector<Diagnostic> AnalyzeTwoFiles(const std::string& file1Uri, const std::string& file1Code,
                                        const std::string& file2Uri, const std::string& file2Code)
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    collector.CollectSymbols({file1Uri, file1Code, &i18n}, parser, table);
    auto diagnostics2 = collector.CollectSymbols({file2Uri, file2Code, &i18n}, parser, table);

    SemanticAnalysisRequest request{table, file2Uri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(file2Code, parser);
    request.sourceCode = file2Code;
    request.tree = parser.Parse(file2Code);

    SemanticAnalyzer analyzer(nullptr);
    auto semDiags = analyzer.Analyze(request);
    diagnostics2.insert(diagnostics2.end(), semDiags.begin(), semDiags.end());

    if (request.tree)
    {
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    }
    return diagnostics2;
}

std::vector<Diagnostic> FilterErrors(const std::vector<Diagnostic>& diagnostics)
{
    std::vector<Diagnostic> errors;
    for (const auto& d : diagnostics)
    {
        if (d.severity == DiagnosticSeverity::Error)
        {
            errors.push_back(d);
        }
    }
    std::sort(errors.begin(), errors.end(),
              [](const Diagnostic& a, const Diagnostic& b)
              {
                  if (a.range.start.line != b.range.start.line)
                  {
                      return a.range.start.line < b.range.start.line;
                  }
                  return a.range.start.character < b.range.start.character;
              });
    return errors;
}

bool HasCode(const std::vector<Diagnostic>& diagnostics, const std::string& code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&code](const Diagnostic& diag) { return diag.code == code; });
}

size_t CountCode(const std::vector<Diagnostic>& diagnostics, const std::string& code)
{
    return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                             [&code](const Diagnostic& diag) { return diag.code == code; }));
}
} // namespace

// =====================================================================================
// Inheritance cycles
// =====================================================================================

// =====================================================================================
// Names inside a mixin body.
//
// A mixin's members are copied into whatever class includes it and compiled there, so an
// unqualified name in a mixin method can be a member of that class - which the mixin cannot see,
// and which may live in a file the workspace has not indexed. Measured on a real Sven Co-op plugin:
// 12 errors in one file, every one of them a member of the including class.
//
// No parity fixture, uniquely among the rules here, and that is worth stating rather than leaving
// as an omission: both harnesses synthesise a class that includes the mixin on its own so that its
// body is compiled at all - asharness calls it _AutoMixinInstantiator_1 and angelscript_oracle
// calls it _OracleMixinProbe_0 - and that synthetic class declares nothing. A corpus file of this
// shape is therefore *rejected* by the compiler under test, and would record the opposite of what
// it means. What the oracle does show, on
//
//     mixin class O5Mix { void Detonate() { O5Arm(); O5Value = 3; } }
//     class O5User : O5Mix { int O5Value; void O5Arm() {} }
//
// is that the only errors name _OracleMixinProbe_0::Detonate. O5User::Detonate is never named,
// which is the compiler saying it compiled clean.
// =====================================================================================
TEST_CASE("ClassRules - A call to the including class's method is not undefined inside a mixin")
{
    const std::string code = "funcdef void MixThink();\n"
                             "class MixHost { void MixArm(MixThink@ f) {} }\n"
                             "mixin class MixShared { void Go() { MixArm(MixThink(Later)); } void Later() {} }\n"
                             "class MixUser : MixHost, MixShared {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-undefined-identifier"));
}

TEST_CASE("ClassRules - The same call outside a mixin is still undefined")
{
    // The other half: silence is scoped to the mixin body and the rule is otherwise untouched.
    // Without this, switching the rule off entirely would pass the test above.
    const std::string code = "class MixPlain { void Go() { MixNeverDeclaredAnywhere(); } }\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-undefined-identifier"));
}

TEST_CASE("ClassRules - Reports a class that inherits from itself")
{
    CHECK(HasCode(AnalyzeClassSnippet("class Loop : Loop {}\n"), "as-err-circular-inherit"));
}

TEST_CASE("ClassRules - Reports a cycle through an intermediate class")
{
    const std::string code = "class A : C {}\n"
                             "class B : A {}\n"
                             "class C : B {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-circular-inherit"));
}

TEST_CASE("ClassRules - Diamond inheritance is not a cycle")
{
    // Regression: the deleted implementation shared one visited set across the whole walk, so
    // reaching a common base down two branches - the shape of every interface diamond - was
    // reported as circular inheritance.
    const std::string code = "interface IBase {}\n"
                             "interface ILeft : IBase {}\n"
                             "interface IRight : IBase {}\n"
                             "class Impl : ILeft, IRight {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-circular-inherit"));
}

TEST_CASE("ClassRules - A deep linear hierarchy is not a cycle")
{
    const std::string code = "class A {}\n"
                             "class B : A {}\n"
                             "class C : B {}\n"
                             "class D : C {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-circular-inherit"));
}

// =====================================================================================
// Base list
// =====================================================================================

TEST_CASE("ClassRules - Reports inheriting from a final class")
{
    const std::string code = "final class Sealed {}\n"
                             "class Derived : Sealed {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-inherit-final"));
}

TEST_CASE("ClassRules - Reports inheriting from more than one class")
{
    const std::string code = "class First {}\n"
                             "class Second {}\n"
                             "class Both : First, Second {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-multi-class-inherit"));
}

TEST_CASE("ClassRules - One class plus several interfaces is allowed")
{
    const std::string code = "class Base {}\n"
                             "interface IOne {}\n"
                             "interface ITwo {}\n"
                             "class Impl : Base, IOne, ITwo {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-multi-class-inherit"));
}

TEST_CASE("ClassRules - A base this analyzer cannot see is never reported")
{
    // The single most important guard in the module. Host application types are registered by the
    // engine and have no declaration in the scripts; the deleted implementation reported every one
    // of them as a missing base.
    const std::string code = "class Weapon : CBaseWeapon {}\n";

    const auto diagnostics = AnalyzeClassSnippet(code);
    CHECK_FALSE(HasCode(diagnostics, "as-err-base-not-found"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-circular-inherit"));
}

// =====================================================================================
// Interface implementation and final overrides
// =====================================================================================

TEST_CASE("ClassRules - Reports an interface method the class never declares")
{
    const std::string code = "interface IThink { void Think(); void Spawn(); }\n"
                             "class Entity : IThink { void Think() {} }\n";

    const auto diagnostics = AnalyzeClassSnippet(code);
    REQUIRE(HasCode(diagnostics, "as-err-interface-impl-missing"));
    CHECK(CountCode(diagnostics, "as-err-interface-impl-missing") == 1);

    const auto missing = std::find_if(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diag)
                                      { return diag.code == "as-err-interface-impl-missing"; });
    CHECK(missing->message.find("Spawn") != std::string::npos);
}

TEST_CASE("ClassRules - An implementation inherited from a base class counts")
{
    // The deleted implementation only looked at direct bases, so a method provided by a grandparent
    // was reported missing.
    const std::string code = "interface IThink { void Think(); }\n"
                             "class Root { void Think() {} }\n"
                             "class Middle : Root {}\n"
                             "class Leaf : Middle, IThink {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-interface-impl-missing"));
}

// This case asserted the opposite - that an abstract class may leave the interface to its
// subclasses - which is what `PROP-07` claimed and what ClassRules implemented. The compiler
// answers `Missing implementation of 'void I::P()'` either way, so the rule was skipping a
// diagnostic it should have emitted. tests/parity/doc_r08_abstract_missing_impl.as.
TEST_CASE("ClassRules - An abstract class must still implement its interface")
{
    const std::string code = "interface IThink { void Think(); }\n"
                             "abstract class Partial : IThink {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-interface-impl-missing"));
}

TEST_CASE("ClassRules - An abstract class that implements its interface is silent")
{
    const std::string code = "interface IThink { void Think(); }\n"
                             "abstract class Partial : IThink { void Think() {} }\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-interface-impl-missing"));
}

// Deferred interface completion: a mixin that names an interface may omit methods;
// the consuming class is required to complete the interface.
TEST_CASE("ClassRules - A mixin naming an interface defers completion to the consuming class")
{
    const std::string codeValid = "interface IThink { void Think(); }\n"
                                  "mixin class Helper : IThink { void Assist() {} }\n"
                                  "class Agent : Helper { void Think() {} }\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(codeValid), "as-err-interface-impl-missing"));

    const std::string codeMissing = "interface IThink { void Think(); }\n"
                                    "mixin class Helper : IThink { void Assist() {} }\n"
                                    "class IncompleteAgent : Helper {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(codeMissing), "as-err-interface-impl-missing"));
}

TEST_CASE("ClassRules - A mixin that implements its interface is silent")
{
    const std::string code = "interface IThink { void Think(); }\n"
                             "mixin class Helper : IThink { void Think() {} }\n"
                             "class Agent : Helper {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-interface-impl-missing"));
}

TEST_CASE("ClassRules - An interface this analyzer cannot see is never judged")
{
    CHECK_FALSE(HasCode(AnalyzeClassSnippet("class Entity : IEngineThing {}\n"), "as-err-interface-impl-missing"));
}

TEST_CASE("ClassRules - Reports replacing a method the base declared final")
{
    const std::string code = "class Base { void Tick() final {} }\n"
                             "class Derived : Base { void Tick() {} }\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-override-final-method"));
}

TEST_CASE("ClassRules - Replacing a non-final base method is allowed")
{
    const std::string code = "class Base { void Tick() {} }\n"
                             "class Derived : Base { void Tick() {} }\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-override-final-method"));
}

// =====================================================================================
// Modifiers
// =====================================================================================

TEST_CASE("ClassRules - Reports a mixin declared final or abstract")
{
    CHECK(HasCode(AnalyzeClassSnippet("mixin final class M {}\n"), "as-err-mixin-final"));
    CHECK(HasCode(AnalyzeClassSnippet("mixin abstract class M {}\n"), "as-err-mixin-abstract"));
}

TEST_CASE("ClassRules - An ordinary mixin is accepted")
{
    const auto diagnostics = AnalyzeClassSnippet("mixin class Helper { void Help() {} }\n");
    CHECK_FALSE(HasCode(diagnostics, "as-err-mixin-final"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-mixin-abstract"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-mixin-child-type"));
}

TEST_CASE("ClassRules - Reports 'external' without 'shared'")
{
    CHECK(HasCode(AnalyzeClassSnippet("external class Lonely;\n"), "as-err-external-not-shared"));
}

TEST_CASE("ClassRules - 'external shared' with no body is the one accepted bodyless form")
{
    const auto diagnostics = AnalyzeClassSnippet("external shared class Known;\n");
    CHECK_FALSE(HasCode(diagnostics, "as-err-external-not-shared"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-declaration-missing-body"));
}

TEST_CASE("ClassRules - Reports a class declared with no body")
{
    CHECK(HasCode(AnalyzeClassSnippet("class Lonely;\n"), "as-err-declaration-missing-body"));
}

TEST_CASE("ClassRules - A method modifier on a class never reaches this module")
{
    // `override` and `explicit` are absent from the grammar's declaration_modifier, which is
    // choice("shared", "external", "abstract", "final"). ClassSignature::modifiers.isOverride is
    // therefore unreachable, and the rule that used to test it was dead code emitting
    // `Syntax error: "Foo"` for a condition that could not occur. Removed; this pins the reason.
    // The class itself still collects cleanly, so nothing else misfires on it either.
    const auto diagnostics = AnalyzeClassSnippet("override class Foo {}\n");
    CHECK_FALSE(HasCode(diagnostics, "as-err-declaration-missing-body"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-external-not-shared"));
}

TEST_CASE("ClassRules - Reports a template class declared in a script")
{
    // A script cannot declare one; the application registers template types. Until the grammar
    // gained the production this parsed with an ERROR node over the `<T>` and the user got a
    // generic syntax error pointing at the angle brackets instead of at the class.
    CHECK(HasCode(AnalyzeClassSnippet("class Holder<T> { T value; }\n"), "as-err-template-class-not-supported"));
}

TEST_CASE("ClassRules - A template class in a predefined stub is accepted")
{
    // Which is what the message says: only allowed in predefined files. `class array<T>` is the
    // first declaration in every stub.
    CHECK_FALSE(
        HasCode(AnalyzeClassSnippet("class array<T> { uint length() const; }\n", "file:///engine.as.predefined"),
                "as-err-template-class-not-supported"));
}

TEST_CASE("ClassRules - A template parameter is not an undeclared identifier")
{
    // `T` is introduced by the declaration, so its uses in the class body resolve. Eight of these
    // fired on the real engine stub before the parameters were collected.
    CHECK_FALSE(HasCode(
        AnalyzeClassSnippet("class array<T> { void insertLast(const T &in v); }\n", "file:///engine.as.predefined"),
        "as-warn-undeclared-identifier"));
}

TEST_CASE("ClassRules - An ordinary class is not reported as a template")
{
    CHECK_FALSE(HasCode(AnalyzeClassSnippet("class Plain { int x; }\n"), "as-err-template-class-not-supported"));
}

TEST_CASE("ClassRules - A predefined stub is exempt from the declaration rules")
{
    // Stubs describe an API the engine already accepted, so how they are written is not this
    // analyzer's business.
    const auto diagnostics = AnalyzeClassSnippet("mixin final class Helper {}\n", "file:///engine.as.predefined");
    CHECK_FALSE(HasCode(diagnostics, "as-err-mixin-final"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-declaration-missing-body"));
}

TEST_CASE("ClassRules - Reports a class named after a reserved word or a built-in type")
{
    CHECK(HasCode(AnalyzeClassSnippet("class int {}\n"), "as-err-reserved-keyword-name"));
    CHECK(HasCode(AnalyzeClassSnippet("class string {}\n"), "as-err-reserved-keyword-name"));
}

TEST_CASE("ClassRules - An ordinary class produces nothing")
{
    const std::string code = "interface IThink { void Think(); }\n"
                             "class Base { int health; }\n"
                             "shared abstract class Entity : Base, IThink\n"
                             "{\n"
                             "    private string m_name;\n"
                             "    void Think() {}\n"
                             "}\n";

    const auto diagnostics = AnalyzeClassSnippet(code);
    for (const auto& diag : diagnostics)
    {
        // Only the rules this module owns; unused-variable and friends are not its business.
        CHECK(diag.code.rfind("as-err-mixin", 0) != 0);
        CHECK(diag.code != "as-err-circular-inherit");
        CHECK(diag.code != "as-err-multi-class-inherit");
        CHECK(diag.code != "as-err-inherit-final");
        CHECK(diag.code != "as-err-declaration-missing-body");
        CHECK(diag.code != "as-err-external-not-shared");
    }
}

TEST_CASE("ClassRules - Only the analysed document is diagnosed")
{
    // A sibling file of the same module is indexed so references resolve, not so its declarations
    // get reported against the file the user opened.
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    collector.CollectSymbols("file:///other.as", "class Loop : Loop {}\n", parser, table);
    collector.CollectSymbols("file:///main.as", "void main() {}\n", parser, table);

    SemanticAnalysisRequest request{table, "file:///main.as", ".as.predefined", &i18n};
    request.sourceCode = "void main() {}\n";

    SemanticAnalyzer analyzer(nullptr);
    CHECK_FALSE(HasCode(analyzer.Analyze(request), "as-err-circular-inherit"));
}

TEST_SUITE("AngelScript_ClassConstructors_Verification")
{
    TEST_CASE("Auto-generation: Suppress default constructor when custom constructor exists")
    {
        const std::string script =
            "class CustomOnly {\n"
            "    CustomOnly(int x, string y) {}\n"
            "}\n"
            "\n"
            "void Main() {\n"
            "    CustomOnly valid(10, \"test\"); // OK: Matches explicit constructor\n"
            "    CustomOnly invalid;           // Error: Default constructor was not synthesized\n"
            "}\n";

        auto rawDiagnostics = AnalyzeClassSnippet(script, "file:///test_suppressed_default_ctor.as");
        auto errors = FilterErrors(rawDiagnostics);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == "as-err-no-default-constructor");
        CHECK(errors[0].range.start.line == 6);
    }

    TEST_CASE("Conversion Constructors: Implicit vs Explicit decorators")
    {
        const std::string script = "class NumberWrapper {\n"
                                   "    int m_val;\n"
                                   "    NumberWrapper(int v) { m_val = v; }             // Implicit conversion\n"
                                   "    NumberWrapper(string s) explicit { m_val = 0; } // Explicit only\n"
                                   "}\n"
                                   "\n"
                                   "void TakeWrapper(NumberWrapper w) {}\n"
                                   "\n"
                                   "void Main() {\n"
                                   "    // 1. Implicit int conversion\n"
                                   "    TakeWrapper(42); // OK: Invokes NumberWrapper(int)\n"
                                   "\n"
                                   "    // 2. Implicit string conversion (Should Fail)\n"
                                   "    TakeWrapper(\"100\"); // Error: Constructor is marked explicit\n"
                                   "\n"
                                   "    // 3. Explicit string construction (Should Pass)\n"
                                   "    TakeWrapper(NumberWrapper(\"100\")); // OK\n"
                                   "}\n";

        auto rawDiagnostics = AnalyzeClassSnippet(script, "file:///test_conversion_ctors.as");
        auto errors = FilterErrors(rawDiagnostics);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == "as-err-no-implicit-conversion");
        CHECK(errors[0].range.start.line == 13);
    }

    TEST_CASE("Deleted Constructors: Explicit deletion of default and copy constructors")
    {
        const std::string script = "class NonInstantiable {\n"
                                   "    NonInstantiable() delete;\n"
                                   "    NonInstantiable(const NonInstantiable &inout) delete;\n"
                                   "}\n"
                                   "\n"
                                   "void Main() {\n"
                                   "    NonInstantiable a; // Error: as-err-deleted-method-called\n"
                                   "}\n";

        auto rawDiagnostics = AnalyzeClassSnippet(script, "file:///test_deleted_ctors.as");
        auto errors = FilterErrors(rawDiagnostics);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == "as-err-deleted-method-called");
        CHECK(errors[0].range.start.line == 6);
    }

    TEST_CASE("Delegation Restriction: Prohibit constructor calling another constructor")
    {
        const std::string script = "class Node {\n"
                                   "    int m_id;\n"
                                   "    Node() {\n"
                                   "        Node(0); // Error: Constructor delegation is prohibited\n"
                                   "    }\n"
                                   "    Node(int id) {\n"
                                   "        m_id = id;\n"
                                   "    }\n"
                                   "}\n";

        auto rawDiagnostics = AnalyzeClassSnippet(script, "file:///test_ctor_delegation.as");
        auto errors = FilterErrors(rawDiagnostics);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == "as-err-constructor-delegation-disallowed");
        CHECK(errors[0].range.start.line == 3);
    }

    TEST_CASE("Copy Constructor: Verification and member initialization order")
    {
        const std::string script = "class Point {\n"
                                   "    int x = 10;\n"
                                   "    int y = 20;\n"
                                   "\n"
                                   "    Point() {}\n"
                                   "    Point(const Point &inout other) {\n"
                                   "        x = other.x;\n"
                                   "        y = other.y;\n"
                                   "    }\n"
                                   "}\n"
                                   "\n"
                                   "void Main() {\n"
                                   "    Point p1;\n"
                                   "    Point p2 = p1; // Resolves to Point(const Point &inout)\n"
                                   "}\n";

        auto rawDiagnostics = AnalyzeClassSnippet(script, "file:///test_copy_ctor.as");
        auto errors = FilterErrors(rawDiagnostics);
        CHECK(errors.empty());
    }
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Class Rules Corpus Audit*"`)
// =====================================================================================

TEST_CASE("ClassRules - Class Rules Corpus Audit" * doctest::skip(true))
{
    if (!angel_lsp::test::CorpusIsAvailable())
    {
        MESSAGE(angel_lsp::test::CorpusMissingMessage());
        return;
    }

    static const std::vector<std::string> k_codes = {
        "as-err-circular-inherit",       "as-err-inherit-final",
        "as-err-multi-class-inherit",    "as-err-mixin-final",
        "as-err-mixin-abstract",         "as-err-mixin-child-type",
        "as-err-reserved-keyword-name",  "as-err-declaration-missing-body",
        "as-err-external-not-shared",    "as-err-template-class-not-supported",
        "as-err-interface-impl-missing", "as-err-override-final-method"};

    const auto result = angel_lsp::test::RunCorpusAudit(
        [](const std::string& code) { return std::find(k_codes.begin(), k_codes.end(), code) != k_codes.end(); });

    MESSAGE("Class-rule corpus audit: files=" << result.filesAnalysed << " totalFlagged=" << result.Total()
                                              << " seconds=" << result.seconds);

    for (const auto& [code, count] : result.countByCode)
    {
        MESSAGE("  " << code << ": " << count);
    }
    for (const auto& hit : result.hits)
    {
        MESSAGE("  " << hit.fileName << ":" << hit.line << " [" << hit.code << "] " << hit.message);
    }

    CHECK(result.filesAnalysed > 0);
}

TEST_CASE("ClassRules - Missing interface method diagnostic underlines only class identifier, not full body")
{
    const std::string ifaceName = angel_lsp::test::GenerateRandomSymbolName("IConfigurable");
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("ASDynamicAmmoConfig");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("Register");
    const std::string helperName = angel_lsp::test::GenerateRandomSymbolName("ValidateAmmunition");

    const std::string code = "interface " + ifaceName + "\n{\n    bool " + methodName + "();\n}\n\n" + "final class " +
                             className + " : " + ifaceName + "\n{\n    void " + helperName +
                             "()\n    {\n        int x = 1;\n        x = 2;\n    }\n}\n";

    const auto diagnostics = AnalyzeClassSnippet(code);
    bool foundInterfaceErr = false;
    for (const auto& d : diagnostics)
    {
        if (d.code == "as-err-interface-impl-missing")
        {
            foundInterfaceErr = true;
            CHECK(d.range.start.line == 5);
            CHECK(d.range.end.line == 5);
            CHECK(d.range.start.character == 12);
            CHECK(d.range.end.character == static_cast<uint32_t>(12 + className.size()));
        }
    }
    CHECK(foundInterfaceErr);
}

TEST_CASE("ClassRules - Missing opening brace before else-if does not synthesize fake else/if/return variables")
{
    const std::string ifaceName = angel_lsp::test::GenerateRandomSymbolName("IConfigurable");
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("ASDynamicAmmoConfig");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("Register");
    const std::string validateName = angel_lsp::test::GenerateRandomSymbolName("ValidateAmmunition");

    const std::string code = "interface " + ifaceName + "\n{\n    bool " + methodName + "();\n}\n\n" + "final class " +
                             className + " : " + ifaceName + "\n{\n    int " + validateName +
                             "(int weapon, int health, int armor)\n    {\n"
                             "        for (int ui = 0; ui < 2; ui++)\n        {\n"
                             "            int idx = 0;\n"
                             "            int type = 0;\n"
                             "            if (weapon != 0)\n\n"
                             "                idx = 1;\n"
                             "                type = 2;\n"
                             "            }\n"
                             "            else if (health > 0)\n"
                             "            {\n"
                             "                type = 3;\n"
                             "            }\n"
                             "            else if (armor > 0)\n"
                             "            {\n"
                             "                type = 4;\n"
                             "            }\n"
                             "            else\n"
                             "            {\n"
                             "                type = 5;\n"
                             "            }\n"
                             "        }\n"
                             "        return 0;\n"
                             "    }\n\n"
                             "    bool " +
                             methodName + "() override\n    {\n        return true;\n    }\n}\n";

    const auto diagnostics = AnalyzeClassSnippet(code);
    bool hasSyntaxError = false;
    for (const auto& d : diagnostics)
    {
        if (d.code == "as-syntax-error" || d.code == "as-syntax-error-missing")
        {
            hasSyntaxError = true;
        }
        // Must never register 'else if' as duplicate variable 'if' or unused variable 'if'
        if (d.code == "as-err-duplicate-symbol" || d.code == "as-warn-unused-variable")
        {
            CHECK(d.message.find("'if'") == std::string::npos);
        }
        // No diagnostic may span across the entire multi-line class body
        if (d.code == "as-err-interface-impl-missing")
        {
            CHECK(d.range.start.line == d.range.end.line);
        }
    }
    CHECK(hasSyntaxError);
}

TEST_CASE("ClassRules - Mixin with final method does not report final override on host class")
{
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("MixFinal");
    const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("HostClass");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("FinalMethod");

    const std::string code = "mixin class " + mixinName +
                             " {\n"
                             "    void " +
                             methodName +
                             "() final {}\n"
                             "}\n"
                             "class " +
                             hostName + " : " + mixinName + " {}\n";

    const auto diags = AnalyzeClassSnippet(code);
    CHECK_FALSE(HasCode(diags, "as-err-override-final-method"));
}

TEST_CASE("ClassRules - Host class can override mixin final method without error")
{
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("MixFinal");
    const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("HostClass");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("FinalMethod");

    const std::string code = "mixin class " + mixinName +
                             " {\n"
                             "    void " +
                             methodName +
                             "() final {}\n"
                             "}\n"
                             "class " +
                             hostName + " : " + mixinName +
                             " {\n"
                             "    void " +
                             methodName +
                             "() {}\n"
                             "}\n";

    const auto diags = AnalyzeClassSnippet(code);
    CHECK_FALSE(HasCode(diags, "as-err-override-final-method"));
}

TEST_CASE("ClassRules - Subclass deriving from host class cannot override final method from mixin")
{
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("MixFinal");
    const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("HostClass");
    const std::string subName = angel_lsp::test::GenerateRandomSymbolName("SubClass");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("FinalMethod");

    const std::string code = "mixin class " + mixinName +
                             " {\n"
                             "    void " +
                             methodName +
                             "() final {}\n"
                             "}\n"
                             "class " +
                             hostName + " : " + mixinName +
                             " {}\n"
                             "class " +
                             subName + " : " + hostName +
                             " {\n"
                             "    void " +
                             methodName +
                             "() {}\n"
                             "}\n";

    const auto diags = AnalyzeClassSnippet(code);
    CHECK(HasCode(diags, "as-err-override-final-method"));
}

TEST_CASE("ClassRules - Mixin property accessor allows this.prop access without error")
{
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("MixProps");
    const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("HostProps");
    const std::string propName = angel_lsp::test::GenerateRandomSymbolName("flare_classname");
    const std::string funcName = angel_lsp::test::GenerateRandomSymbolName("flare_create");

    const std::string code = "mixin class " + mixinName +
                             " {\n"
                             "    const string& get_" +
                             propName +
                             "() const property {\n"
                             "        return \"test\";\n"
                             "    }\n"
                             "    void " +
                             funcName +
                             "() {\n"
                             "        string s = this." +
                             propName +
                             ";\n"
                             "    }\n"
                             "}\n"
                             "class " +
                             hostName + " : " + mixinName + " {}\n";

    const auto diags = AnalyzeClassSnippet(code);
    CHECK_FALSE(HasCode(diags, "as-err-mixin-instantiation-member-not-found"));
}

TEST_CASE("ClassRules - Mixin referencing undeclared identifier emits as-err-mixin-instantiation-member-not-found")
{
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("MixMissing");
    const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("HostMissing");
    const std::string funcName = angel_lsp::test::GenerateRandomSymbolName("DoInit");
    const std::string undeclaredSelf = angel_lsp::test::GenerateRandomSymbolName("self");
    const std::string undeclaredConfig = angel_lsp::test::GenerateRandomSymbolName("config");

    const std::string code = "mixin class " + mixinName +
                             " {\n"
                             "    void " +
                             funcName +
                             "() {\n"
                             "        " +
                             undeclaredSelf +
                             ".DoSomething();\n"
                             "        " +
                             undeclaredConfig +
                             " = 42;\n"
                             "    }\n"
                             "}\n"
                             "class " +
                             hostName + " : " + mixinName + " {}\n";

    const auto diags = AnalyzeClassSnippet(code);
    CHECK(HasCode(diags, "as-err-mixin-instantiation-member-not-found"));
    size_t count = CountCode(diags, "as-err-mixin-instantiation-member-not-found");
    CHECK(count >= 2);
}

TEST_CASE("ClassRules - Mixin class cannot inherit from regular class (Probe C-i)")
{
    const std::string regularBase = angel_lsp::test::GenerateRandomSymbolName("RegularBase");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("InvalidMixin");

    const std::string code =
        "class " + regularBase + " {}\n" + "mixin class " + mixinName + " : " + regularBase + " {}\n";

    const auto diags = AnalyzeClassSnippet(code);
    const auto errors = FilterErrors(diags);
    CHECK(HasCode(errors, "as-err-mixin-inherit-class"));
}

TEST_CASE("ClassRules - Mixin can omit interface method and concrete class must implement it (Probe C-ii)")
{
    const std::string ifaceName = angel_lsp::test::GenerateRandomSymbolName("IService");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("Serve");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("ServiceMixin");
    const std::string incompleteHost = angel_lsp::test::GenerateRandomSymbolName("IncompleteHost");
    const std::string completeHost = angel_lsp::test::GenerateRandomSymbolName("CompleteHost");

    const std::string mixinCode = "interface " + ifaceName + " { void " + methodName + "(); }\n" + "mixin class " +
                                  mixinName + " : " + ifaceName + " {}\n";

    const auto mixinDiags = AnalyzeClassSnippet(mixinCode);
    const auto mixinErrors = FilterErrors(mixinDiags);
    CHECK_FALSE(HasCode(mixinErrors, "as-err-interface-impl-missing"));

    const std::string incompleteCode = mixinCode + "class " + incompleteHost + " : " + mixinName + " {}\n";
    const auto incompleteDiags = AnalyzeClassSnippet(incompleteCode);
    const auto incompleteErrors = FilterErrors(incompleteDiags);
    CHECK(HasCode(incompleteErrors, "as-err-interface-impl-missing"));

    const std::string completeCode =
        mixinCode + "class " + completeHost + " : " + mixinName + " {\n" + "    void " + methodName + "() {}\n" + "}\n";
    const auto completeDiags = AnalyzeClassSnippet(completeCode);
    const auto completeErrors = FilterErrors(completeDiags);
    CHECK_FALSE(HasCode(completeErrors, "as-err-interface-impl-missing"));
}

TEST_CASE("ClassRules - Overloaded method with different parameters is allowed when base method is final")
{
    const std::string baseName = angel_lsp::test::GenerateRandomSymbolName("BaseOverload");
    const std::string derivedName = angel_lsp::test::GenerateRandomSymbolName("DerivedOverload");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("FinalMethod");

    const std::string codeLegal = "class " + baseName +
                                  " {\n"
                                  "    void " +
                                  methodName +
                                  "(int a) final {}\n"
                                  "}\n"
                                  "class " +
                                  derivedName + " : " + baseName +
                                  " {\n"
                                  "    void " +
                                  methodName +
                                  "(string s) {}\n"
                                  "}\n";

    const auto diagsLegal = AnalyzeClassSnippet(codeLegal);
    CHECK_FALSE(HasCode(FilterErrors(diagsLegal), "as-err-override-final-method"));

    const std::string codeIllegal = "class " + baseName +
                                    "2 {\n"
                                    "    void " +
                                    methodName +
                                    "(int a) final {}\n"
                                    "}\n"
                                    "class " +
                                    derivedName + "2 : " + baseName +
                                    "2 {\n"
                                    "    void " +
                                    methodName +
                                    "(int a) {}\n"
                                    "}\n";

    const auto diagsIllegal = AnalyzeClassSnippet(codeIllegal);
    CHECK(HasCode(FilterErrors(diagsIllegal), "as-err-override-final-method"));
}

TEST_CASE("ClassRules - Virtual property block with get final flags override on derived property")
{
    const std::string baseName = angel_lsp::test::GenerateRandomSymbolName("BaseProp");
    const std::string derivedName = angel_lsp::test::GenerateRandomSymbolName("DerivedProp");
    const std::string propName = angel_lsp::test::GenerateRandomSymbolName("prop_val");

    const std::string code = "class " + baseName +
                             " {\n"
                             "    string " +
                             propName +
                             " { get final { return \"base\"; } }\n"
                             "}\n"
                             "class " +
                             derivedName + " : " + baseName +
                             " {\n"
                             "    string " +
                             propName +
                             " { get { return \"derived\"; } }\n"
                             "}\n";

    const auto diags = AnalyzeClassSnippet(code);
    CHECK(HasCode(FilterErrors(diags), "as-err-override-final-method"));
}

TEST_CASE("ClassRules - Multi-level inheritance cites exact grand-base declaring final method")
{
    const std::string grandName = angel_lsp::test::GenerateRandomSymbolName("GrandBase");
    const std::string baseName = angel_lsp::test::GenerateRandomSymbolName("MidBase");
    const std::string derivedName = angel_lsp::test::GenerateRandomSymbolName("SubDerived");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("DoWork");

    const std::string code = "class " + grandName +
                             " {\n"
                             "    void " +
                             methodName +
                             "() final {}\n"
                             "}\n"
                             "class " +
                             baseName + " : " + grandName +
                             " {}\n"
                             "class " +
                             derivedName + " : " + baseName +
                             " {\n"
                             "    void " +
                             methodName +
                             "() {}\n"
                             "}\n";

    const auto diags = AnalyzeClassSnippet(code);
    const auto errors = FilterErrors(diags);
    CHECK(HasCode(errors, "as-err-override-final-method"));
    bool citedGrand = false;
    for (const auto& err : errors)
    {
        if (err.code == "as-err-override-final-method" && err.message.find(grandName) != std::string::npos)
        {
            citedGrand = true;
            break;
        }
    }
    CHECK(citedGrand);
}

TEST_CASE("ClassRules - Multi-file cross-#include does not leak final method override diagnostics to foreign file")
{
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("ASWFlareConfig");
    const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("weapon_bts_flare");
    const std::string propName = angel_lsp::test::GenerateRandomSymbolName("flare_classname");

    // Generate schemaCode with padding lines so the final method sits on line 180
    std::string schemaCode = "// Schema file\n";
    for (int i = 0; i < 178; ++i)
    {
        schemaCode += "// padding line\n";
    }
    schemaCode += "mixin class " + mixinName +
                  " {\n"
                  "    const string& get_" +
                  propName +
                  "() const property final { return \"test\"; }\n"
                  "}\n";

    // Generate flareCode with code on line 180
    std::string flareCode = "class " + hostName + " : " + mixinName + " {\n";
    for (int i = 0; i < 178; ++i)
    {
        flareCode += "    int pad_" + std::to_string(i) + " = 0;\n";
    }
    flareCode += "    void Holster() {\n"
                 "        int m_iAmmoSave = 100;\n"
                 "        m_iAmmoSave++;\n"
                 "    }\n"
                 "}\n";

    const auto diags = AnalyzeTwoFiles("file:///schema.as", schemaCode, "file:///flare.as", flareCode);
    CHECK_FALSE(HasCode(diags, "as-err-override-final-method"));
    for (const auto& d : diags)
    {
        CHECK(d.fileUri == "file:///flare.as");
        CHECK_FALSE(d.code == "as-err-override-final-method");
    }
}

TEST_CASE("ClassRules - Global ::Deploy inside a mixin is undefined if not declared at global scope")
{
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("MixShared");
    const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("MixUser");
    const std::string calleeName = angel_lsp::test::GenerateRandomSymbolName("Deploy");

    const std::string code = "mixin class " + mixinName + " { void Go() { ::" + calleeName + "(); } }\n" + "class " +
                             hostName + " : " + mixinName + " {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-undefined-identifier"));
}

TEST_CASE("ClassRules - Calling ::UnknownNS::Foo emits as-err-undefined-namespace")
{
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("UnknownNS");
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Foo");

    const std::string code = "void main() {\n"
                             "    ::" +
                             nsName + "::" + fnName +
                             "();\n"
                             "}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-undefined-namespace"));
}
