#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "analysis/rules/ClassRules.h"
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

namespace
{
    /** @brief Runs the full analyzer over a snippet and returns its diagnostics. */
    std::vector<Diagnostic> AnalyzeClassSnippet(const std::string &code,
                                                const std::string &fileUri = "file:///classes.as")
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

        SemanticAnalyzer analyzer(nullptr);
        return analyzer.Analyze(request);
    }

    bool HasCode(const std::vector<Diagnostic> &diagnostics, const std::string &code)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&code](const Diagnostic &diag) { return diag.code == code; });
    }

    size_t CountCode(const std::vector<Diagnostic> &diagnostics, const std::string &code)
    {
        return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                                 [&code](const Diagnostic &diag) { return diag.code == code; }));
    }
}

// =====================================================================================
// Inheritance cycles
// =====================================================================================

TEST_CASE("ClassRules - Reports a class that inherits from itself")
{
    CHECK(HasCode(AnalyzeClassSnippet("class Loop : Loop {}\n"), "as-err-circular-inherit"));
}

TEST_CASE("ClassRules - Reports a cycle through an intermediate class")
{
    const std::string code =
        "class A : C {}\n"
        "class B : A {}\n"
        "class C : B {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-circular-inherit"));
}

TEST_CASE("ClassRules - Diamond inheritance is not a cycle")
{
    // Regression: the deleted implementation shared one visited set across the whole walk, so
    // reaching a common base down two branches - the shape of every interface diamond - was
    // reported as circular inheritance.
    const std::string code =
        "interface IBase {}\n"
        "interface ILeft : IBase {}\n"
        "interface IRight : IBase {}\n"
        "class Impl : ILeft, IRight {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-circular-inherit"));
}

TEST_CASE("ClassRules - A deep linear hierarchy is not a cycle")
{
    const std::string code =
        "class A {}\n"
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
    const std::string code =
        "final class Sealed {}\n"
        "class Derived : Sealed {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-inherit-final"));
}

TEST_CASE("ClassRules - Reports inheriting from more than one class")
{
    const std::string code =
        "class First {}\n"
        "class Second {}\n"
        "class Both : First, Second {}\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-multi-class-inherit"));
}

TEST_CASE("ClassRules - One class plus several interfaces is allowed")
{
    const std::string code =
        "class Base {}\n"
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
    const std::string code =
        "interface IThink { void Think(); void Spawn(); }\n"
        "class Entity : IThink { void Think() {} }\n";

    const auto diagnostics = AnalyzeClassSnippet(code);
    REQUIRE(HasCode(diagnostics, "as-err-interface-impl-missing"));
    CHECK(CountCode(diagnostics, "as-err-interface-impl-missing") == 1);

    const auto missing = std::find_if(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic &diag) { return diag.code == "as-err-interface-impl-missing"; });
    CHECK(missing->message.find("Spawn") != std::string::npos);
}

TEST_CASE("ClassRules - An implementation inherited from a base class counts")
{
    // The deleted implementation only looked at direct bases, so a method provided by a grandparent
    // was reported missing.
    const std::string code =
        "interface IThink { void Think(); }\n"
        "class Root { void Think() {} }\n"
        "class Middle : Root {}\n"
        "class Leaf : Middle, IThink {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-interface-impl-missing"));
}

TEST_CASE("ClassRules - An abstract class may leave the interface to its subclasses")
{
    const std::string code =
        "interface IThink { void Think(); }\n"
        "abstract class Partial : IThink {}\n";

    CHECK_FALSE(HasCode(AnalyzeClassSnippet(code), "as-err-interface-impl-missing"));
}

TEST_CASE("ClassRules - An interface this analyzer cannot see is never judged")
{
    CHECK_FALSE(HasCode(AnalyzeClassSnippet("class Entity : IEngineThing {}\n"),
                        "as-err-interface-impl-missing"));
}

TEST_CASE("ClassRules - Reports replacing a method the base declared final")
{
    const std::string code =
        "class Base { void Tick() final {} }\n"
        "class Derived : Base { void Tick() {} }\n";

    CHECK(HasCode(AnalyzeClassSnippet(code), "as-err-override-final-method"));
}

TEST_CASE("ClassRules - Replacing a non-final base method is allowed")
{
    const std::string code =
        "class Base { void Tick() {} }\n"
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
    CHECK(HasCode(AnalyzeClassSnippet("external class Lonely;\n"), "as-syntax-error"));
}

TEST_CASE("ClassRules - 'external shared' with no body is the one accepted bodyless form")
{
    CHECK_FALSE(HasCode(AnalyzeClassSnippet("external shared class Known;\n"), "as-syntax-error"));
}

TEST_CASE("ClassRules - A template class is left to the parser to report")
{
    // The grammar has no production for a template class declaration, so `<T>` parses as an ERROR
    // node that the parser pass already reports. ClassSignature::isTemplate is never set, which is
    // why as-err-template-class-not-supported is deliberately not implemented in this module.
    CHECK_FALSE(HasCode(AnalyzeClassSnippet("class Holder<T> { T value; }\n"),
                        "as-err-template-class-not-supported"));
}

TEST_CASE("ClassRules - A predefined stub is exempt from the declaration rules")
{
    // Stubs describe an API the engine already accepted, so how they are written is not this
    // analyzer's business.
    const auto diagnostics = AnalyzeClassSnippet("mixin final class Helper {}\n",
                                                 "file:///engine.as.predefined");
    CHECK_FALSE(HasCode(diagnostics, "as-err-mixin-final"));
    CHECK_FALSE(HasCode(diagnostics, "as-syntax-error"));
}

TEST_CASE("ClassRules - Reports a class named after a reserved word or a built-in type")
{
    CHECK(HasCode(AnalyzeClassSnippet("class int {}\n"), "as-err-reserved-keyword-name"));
    CHECK(HasCode(AnalyzeClassSnippet("class string {}\n"), "as-err-reserved-keyword-name"));
}

TEST_CASE("ClassRules - An ordinary class produces nothing")
{
    const std::string code =
        "interface IThink { void Think(); }\n"
        "class Base { int health; }\n"
        "shared abstract class Entity : Base, IThink\n"
        "{\n"
        "    private string m_name;\n"
        "    void Think() {}\n"
        "}\n";

    const auto diagnostics = AnalyzeClassSnippet(code);
    for (const auto &diag : diagnostics)
    {
        // Only the rules this module owns; unused-variable and friends are not its business.
        CHECK(diag.code.rfind("as-err-mixin", 0) != 0);
        CHECK(diag.code != "as-err-circular-inherit");
        CHECK(diag.code != "as-err-multi-class-inherit");
        CHECK(diag.code != "as-err-inherit-final");
        CHECK(diag.code != "as-syntax-error");
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

    SemanticAnalysisRequest request{ table, "file:///main.as", ".as.predefined", &i18n };
    request.sourceCode = "void main() {}\n";

    SemanticAnalyzer analyzer(nullptr);
    CHECK_FALSE(HasCode(analyzer.Analyze(request), "as-err-circular-inherit"));
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Class Rules Corpus Audit*"`)
// =====================================================================================

TEST_CASE("ClassRules - Class Rules Corpus Audit" * doctest::skip(true))
{
    static const std::vector<std::string> k_codes = {
        "as-err-circular-inherit", "as-err-inherit-final", "as-err-multi-class-inherit",
        "as-err-mixin-final", "as-err-mixin-abstract", "as-err-mixin-child-type",
        "as-err-reserved-keyword-name", "as-syntax-error",
        "as-err-interface-impl-missing", "as-err-override-final-method"
    };

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return std::find(k_codes.begin(), k_codes.end(), code) != k_codes.end();
    });

    MESSAGE("Class-rule corpus audit: files=" << result.filesAnalysed
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
