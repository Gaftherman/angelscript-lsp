#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    std::vector<Symbol> CollectFunctionCandidates(const std::string &code, const std::string &funcName, SymbolTable &table)
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        const std::string fileUri = "file:///overload_test.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        auto found = table.FindSymbols(funcName);
        std::vector<Symbol> candidates;
        for (const auto &sym : found)
        {
            if (sym.type == SymbolType::Function)
            {
                candidates.push_back(sym);
            }
        }
        return candidates;
    }
}

TEST_SUITE("OverloadResolution")
{
    TEST_CASE("Prefers Exact Match Over Widening")
    {
        std::string code =
            "void Process(int x) { }\n"
            "void Process(double x) { }\n";

        SymbolTable table;
        auto candidates = CollectFunctionCandidates(code, "Process", table);
        REQUIRE(candidates.size() == 2);

        auto matchInt = ResolveBestOverload(candidates, { "int" }, table);
        REQUIRE(matchInt.bestCandidate != nullptr);
        CHECK(matchInt.bestCandidate->GetFunction().parameters[0].typeName == "int");

        auto matchDouble = ResolveBestOverload(candidates, { "double" }, table);
        REQUIRE(matchDouble.bestCandidate != nullptr);
        CHECK(matchDouble.bestCandidate->GetFunction().parameters[0].typeName == "double");
    }

    TEST_CASE("Inheritance Derived Over Base Match")
    {
        std::string code =
            "class Animal { }\n"
            "class Dog : Animal { }\n"
            "void Feed(Animal@ a) { }\n"
            "void Feed(Dog@ d) { }\n";

        SymbolTable table;
        auto candidates = CollectFunctionCandidates(code, "Feed", table);
        REQUIRE(candidates.size() == 2);

        auto matchDog = ResolveBestOverload(candidates, { "Dog@" }, table);
        REQUIRE(matchDog.bestCandidate != nullptr);
        CHECK(matchDog.bestCandidate->GetFunction().parameters[0].typeName == "Dog@");

        auto matchAnimal = ResolveBestOverload(candidates, { "Animal@" }, table);
        REQUIRE(matchAnimal.bestCandidate != nullptr);
        CHECK(matchAnimal.bestCandidate->GetFunction().parameters[0].typeName == "Animal@");
    }

    TEST_CASE("Const Reference Qualification")
    {
        std::string code =
            "void Log(string s) { }\n"
            "void Log(const string &in s) { }\n";

        SymbolTable table;
        auto candidates = CollectFunctionCandidates(code, "Log", table);
        REQUIRE(candidates.size() == 2);

        auto matchConst = ResolveBestOverload(candidates, { "const string" }, table);
        REQUIRE(matchConst.bestCandidate != nullptr);
        CHECK(matchConst.bestCandidate->GetFunction().parameters[0].isConst);
        CHECK(matchConst.bestCandidate->GetFunction().parameters[0].modifier == ParameterModifier::In);
    }

    TEST_CASE("Prefers Exact Arity Over Default Arguments")
    {
        std::string code =
            "void Compute(int a) { }\n"
            "void Compute(int a, int b = 0) { }\n";

        SymbolTable table;
        auto candidates = CollectFunctionCandidates(code, "Compute", table);
        REQUIRE(candidates.size() == 2);

        auto match = ResolveBestOverload(candidates, { "int" }, table);
        REQUIRE(match.bestCandidate != nullptr);
        CHECK(match.bestCandidate->GetFunction().parameters.size() == 1);
    }

    TEST_CASE("Detects Ambiguous Overloads")
    {
        std::string code =
            "void Action(int a, double b) { }\n"
            "void Action(double a, int b) { }\n";

        SymbolTable table;
        auto candidates = CollectFunctionCandidates(code, "Action", table);
        REQUIRE(candidates.size() == 2);

        // Passing two ints -> both require one widening conversion -> equal score
        auto match = ResolveBestOverload(candidates, { "int", "int" }, table);
        CHECK(match.isAmbiguous);
    }
}

// =====================================================================================
// `int32` / `uint32` are the explicit spellings of `int` / `uint`, and a source file may write
// either - nothing canonicalises them away. The overload resolver used to carry its own list of
// integer primitives that omitted the explicit forms while the conversion rules used one that
// included them, so the two classified the same type differently.
//
// This is a characterisation test, not a regression test: it passes against the old lists too. The
// divergence never surfaced because the silent-unless-fully-visible policy absorbed it - a call
// whose overloads will not resolve is passed over rather than reported - so it cost a diagnostic
// rather than producing a wrong one. What this pins down is that the explicit spellings must not
// start producing a *false* diagnostic now that both sides share one vocabulary.
//
// The real AngelScript compiler accepts the snippet below without a word.
// =====================================================================================

TEST_CASE("Overload resolution - Explicit int32/uint32 spellings rank like int/uint")
{
    const char *script = R"(
        void Take(float v) {}
        void Take(int64 v) {}

        void main()
        {
            int32 explicitInt = 7;
            Take(explicitInt);

            uint32 explicitUint = 8;
            Take(explicitUint);
        }
    )";

    auto doc = angel_lsp::test::CreateTestDocument("file:///int32_overload.as", script);
    REQUIRE(static_cast<bool>(doc));

    const auto diagnostics = doc->GetDiagnostics();
    for (const auto &d : diagnostics)
    {
        CAPTURE(d.code);
        CAPTURE(d.message);
        CHECK(d.code != "as-err-call-no-matching-signature");
        CHECK(d.code != "as-err-call-ambiguous");
    }
}
