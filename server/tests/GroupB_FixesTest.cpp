#include <doctest/doctest.h>
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include "i18n/i18n.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

static bool TestSemanticCode(const std::string &sourceCode, const std::string &expectedCode = "")
{
    std::string fileUri = "file:///test_suite.as";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto syntaxDiags = collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto semanticDiags = analyzer.Analyze(req);

    if (expectedCode.empty())
    {
        return syntaxDiags.empty() && semanticDiags.empty();
    }

    for (const auto &d : syntaxDiags)
    {
        if (d.code == expectedCode) return true;
    }
    for (const auto &d : semanticDiags)
    {
        if (d.code == expectedCode) return true;
    }
    return false;
}

TEST_CASE("Group B1: Mixin as Base Class")
{
    SUBCASE("Variant 1: Single mixin inheritance")
    {
        std::string code = "mixin class Loggable { void Log() {} }\nclass Widget : Loggable { void Run() {} }";
        CHECK(TestSemanticCode(code, ""));
    }

    SUBCASE("Variant 2: Class extending mixin with virtual methods")
    {
        std::string code = "mixin class Renderable { void Draw() {} }\nclass Sprite : Renderable { void Render() {} }";
        CHECK(TestSemanticCode(code, ""));
    }

    SUBCASE("Variant 3: Class including multiple mixins")
    {
        std::string code = "mixin class Serializable { void Save() {} }\nmixin class Updatable { void Update() {} }\nclass Actor : Serializable, Updatable { void Act() {} }";
        CHECK(TestSemanticCode(code, ""));
    }
}

TEST_CASE("Group B2: Break and Continue Control Flow Validation")
{
    SUBCASE("Variant 1: Break outside loop or switch")
    {
        std::string code = "void test() { break; }";
        CHECK(TestSemanticCode(code, "as-err-break-outside-loop"));
    }

    SUBCASE("Variant 2: Continue outside loop")
    {
        std::string code = "void test() { continue; }";
        CHECK(TestSemanticCode(code, "as-err-continue-outside-loop"));
    }

    SUBCASE("Variant 3: Continue inside standalone switch")
    {
        std::string code = "void test(int s) { switch(s) { case 1: continue; } }";
        CHECK(TestSemanticCode(code, "as-err-continue-outside-loop"));
    }

    SUBCASE("Variant 4: Valid break and continue inside loop")
    {
        std::string code = "void test() { for (int i = 0; i < 10; ++i) { if (i == 5) break; else continue; } }";
        CHECK(TestSemanticCode(code, ""));
    }
}

TEST_CASE("Group B3: Switch and Case Validation")
{
    SUBCASE("Variant 1: Float case label")
    {
        std::string code = "void test(float x) { switch(int(x)) { case 1.5f: break; } }";
        CHECK(TestSemanticCode(code, "as-err-invalid-case-type"));
    }

    SUBCASE("Variant 2: Duplicate case value")
    {
        std::string code = "void test(int s) { switch(s) { case 1: break; case 1: break; } }";
        CHECK(TestSemanticCode(code, "as-err-duplicate-case-value"));
    }

    SUBCASE("Variant 3: String case label")
    {
        std::string code = "void test(int s) { switch(s) { case \"hello\": break; } }";
        CHECK(TestSemanticCode(code, "as-err-invalid-case-type"));
    }

    SUBCASE("Variant 4: Valid enum case switch")
    {
        std::string code = "enum State { IDLE, RUNNING }\nvoid test(State s) { switch(s) { case IDLE: break; case RUNNING: break; } }";
        CHECK(TestSemanticCode(code, ""));
    }
}

TEST_CASE("Group B4: cast<T>() Validation")
{
    SUBCASE("Variant 1: Valid reference cast")
    {
        std::string code = "class Base {}\nclass Derived : Base {}\nvoid test(Base@ b) { Derived@ d = cast<Derived>(b); }";
        CHECK(TestSemanticCode(code, ""));
    }

    SUBCASE("Variant 2: Primitive cast attempt")
    {
        std::string code = "void test(int a) { float b = cast<float>(a); }";
        CHECK(TestSemanticCode(code, "as-err-unresolved-type"));
    }

    SUBCASE("Variant 3: Auto cast attempt")
    {
        std::string code = "void test(int a) { auto b = cast<auto>(a); }";
        CHECK(TestSemanticCode(code, "as-err-unresolved-type"));
    }

    SUBCASE("Variant 4: Unresolved type cast attempt")
    {
        std::string code = "void test(int a) { NonExistent@ b = cast<NonExistent>(a); }";
        CHECK(TestSemanticCode(code, "as-err-unresolved-type"));
    }
}
