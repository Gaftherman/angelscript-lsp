#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include <random>

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

TEST_CASE("OverloadResolution - Prefers Exact Match Over Widening")
{
    std::mt19937_64 rng(0x1337BEEF);
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Process");
    std::string code =
        "void " + fnName + "(int x) { }\n" +
        "void " + fnName + "(double x) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnName, table);
    REQUIRE(candidates.size() == 2);

    auto matchInt = ResolveBestOverload(candidates, { "int" }, table);
    REQUIRE(matchInt.bestCandidate != nullptr);
    CHECK(matchInt.bestCandidate->GetFunction().parameters[0].typeName == "int");

    auto matchDouble = ResolveBestOverload(candidates, { "double" }, table);
    REQUIRE(matchDouble.bestCandidate != nullptr);
    CHECK(matchDouble.bestCandidate->GetFunction().parameters[0].typeName == "double");
}

TEST_CASE("OverloadResolution - Inheritance Derived Over Base Match")
{
    std::mt19937_64 rng(0x1337BEF0);
    const std::string baseCls = angel_lsp::test::GenerateIdentifier(rng, "BaseCls");
    const std::string derivedCls = angel_lsp::test::GenerateIdentifier(rng, "DerivedCls");
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Feed");

    std::string code =
        "class " + baseCls + " { }\n" +
        "class " + derivedCls + " : " + baseCls + " { }\n" +
        "void " + fnName + "(" + baseCls + "@ a) { }\n" +
        "void " + fnName + "(" + derivedCls + "@ d) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnName, table);
    REQUIRE(candidates.size() == 2);

    auto matchDerived = ResolveBestOverload(candidates, { derivedCls + "@" }, table);
    REQUIRE(matchDerived.bestCandidate != nullptr);
    CHECK(matchDerived.bestCandidate->GetFunction().parameters[0].typeName == (derivedCls + "@"));

    auto matchBase = ResolveBestOverload(candidates, { baseCls + "@" }, table);
    REQUIRE(matchBase.bestCandidate != nullptr);
    CHECK(matchBase.bestCandidate->GetFunction().parameters[0].typeName == (baseCls + "@"));
}

TEST_CASE("OverloadResolution - Const Reference Qualification")
{
    std::mt19937_64 rng(0x1337BEF1);
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Log");

    std::string code =
        "void " + fnName + "(string s) { }\n" +
        "void " + fnName + "(const string &in s) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnName, table);
    REQUIRE(candidates.size() == 2);

    auto matchConst = ResolveBestOverload(candidates, { "const string" }, table);
    REQUIRE(matchConst.bestCandidate != nullptr);
    CHECK(matchConst.bestCandidate->GetFunction().parameters[0].isConst);
    CHECK(matchConst.bestCandidate->GetFunction().parameters[0].modifier == ParameterModifier::In);
}

TEST_CASE("OverloadResolution - Prefers Exact Arity Over Default Arguments")
{
    std::mt19937_64 rng(0x1337BEF2);
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Compute");

    std::string code =
        "void " + fnName + "(int a) { }\n" +
        "void " + fnName + "(int a, int b = 0) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnName, table);
    REQUIRE(candidates.size() == 2);

    auto match = ResolveBestOverload(candidates, { "int" }, table);
    REQUIRE(match.bestCandidate != nullptr);
    CHECK(match.bestCandidate->GetFunction().parameters.size() == 1);
}

TEST_CASE("OverloadResolution - Detects Ambiguous Overloads")
{
    std::mt19937_64 rng(0x1337BEF3);
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Action");

    std::string code =
        "void " + fnName + "(int a, double b) { }\n" +
        "void " + fnName + "(double a, int b) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnName, table);
    REQUIRE(candidates.size() == 2);

    // Passing two ints -> both require one widening conversion -> equal score
    auto match = ResolveBestOverload(candidates, { "int", "int" }, table);
    CHECK(match.isAmbiguous);
}

TEST_CASE("OverloadResolution - Multi-argument Pareto dominance chooses candidate strictly better across arguments")
{
    std::mt19937_64 rng(0x1337BEF4);
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Process");

    std::string code =
        "void " + fnName + "(int a, int b) { }\n" +
        "void " + fnName + "(int a, double b) { }\n" +
        "void " + fnName + "(double a, double b) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnName, table);
    REQUIRE(candidates.size() == 3);

    auto match = ResolveBestOverload(candidates, { "int", "int" }, table);
    REQUIRE(match.bestCandidate != nullptr);
    CHECK_FALSE(match.isAmbiguous);
    CHECK(match.bestCandidate->GetFunction().parameters[0].typeName == "int");
    CHECK(match.bestCandidate->GetFunction().parameters[1].typeName == "int");
    REQUIRE(match.bestCostVector.size() == 2);
    CHECK(match.bestCostVector[0] == 0); // Exact
    CHECK(match.bestCostVector[1] == 0); // Exact
}

TEST_CASE("OverloadResolution - Multi-argument cost vector identical tie broken by fewer default arguments")
{
    std::mt19937_64 rng(0x1337BEF5);
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Calc");

    std::string code =
        "void " + fnName + "(int a, int b) { }\n" +
        "void " + fnName + "(int a, int b, int c = 0) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnName, table);
    REQUIRE(candidates.size() == 2);

    auto match = ResolveBestOverload(candidates, { "int", "int" }, table);
    REQUIRE(match.bestCandidate != nullptr);
    CHECK_FALSE(match.isAmbiguous);
    CHECK(match.bestCandidate->GetFunction().parameters.size() == 2);
    REQUIRE(match.bestCostVector.size() == 2);
    CHECK(match.bestCostVector[0] == 0);
    CHECK(match.bestCostVector[1] == 0);
}

TEST_CASE("OverloadResolution - Handle to Reference Binding Exact Match")
{
    std::mt19937_64 rng(0x1337BEF6);
    const std::string clsName = angel_lsp::test::GenerateIdentifier(rng, "Entity");
    const std::string fnProcess = angel_lsp::test::GenerateIdentifier(rng, "Process");
    const std::string fnInspect = angel_lsp::test::GenerateIdentifier(rng, "Inspect");

    std::string code =
        "class " + clsName + " { }\n" +
        "void " + fnProcess + "(" + clsName + "& inout e) { }\n" +
        "void " + fnInspect + "(const " + clsName + "& in e) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnProcess, table);
    REQUIRE(candidates.size() == 1);

    auto match = ResolveBestOverload(candidates, { clsName + "@" }, table);
    REQUIRE(match.bestCandidate != nullptr);
    CHECK_FALSE(match.isAmbiguous);
    REQUIRE(match.bestCostVector.size() == 1);
    CHECK(match.bestCostVector[0] == 0); // Exact match

    SymbolTable inspectTable;
    auto inspectCandidates = CollectFunctionCandidates(code, fnInspect, inspectTable);
    REQUIRE(inspectCandidates.size() == 1);
    auto matchInspect = ResolveBestOverload(inspectCandidates, { clsName + "@" }, inspectTable);
    REQUIRE(matchInspect.bestCandidate != nullptr);
    CHECK_FALSE(matchInspect.isAmbiguous);
    REQUIRE(matchInspect.bestCostVector.size() == 1);
    CHECK(matchInspect.bestCostVector[0] == 0); // Exact match
}

TEST_CASE("OverloadResolution - Const Handle to Mutable Reference is Incompatible")
{
    std::mt19937_64 rng(0x1337BEF7);
    const std::string clsName = angel_lsp::test::GenerateIdentifier(rng, "Entity");
    const std::string fnProcess = angel_lsp::test::GenerateIdentifier(rng, "Process");

    std::string code =
        "class " + clsName + " { }\n" +
        "void " + fnProcess + "(" + clsName + "& inout e) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnProcess, table);
    REQUIRE(candidates.size() == 1);

    auto match = ResolveBestOverload(candidates, { "const " + clsName + "@" }, table);
    CHECK(match.bestCandidate == nullptr);
}

TEST_CASE("OverloadResolution - Competing Handle vs Reference Overload is Ambiguous (asharness parity)")
{
    std::mt19937_64 rng(0x1337BEF8);
    const std::string clsName = angel_lsp::test::GenerateIdentifier(rng, "Foo");
    const std::string fnProcess = angel_lsp::test::GenerateIdentifier(rng, "Process");

    std::string code =
        "class " + clsName + " { }\n" +
        "void " + fnProcess + "(" + clsName + "@ h) { }\n" +
        "void " + fnProcess + "(" + clsName + "& inout r) { }\n";

    SymbolTable table;
    auto candidates = CollectFunctionCandidates(code, fnProcess, table);
    REQUIRE(candidates.size() == 2);

    auto matchHandle = ResolveBestOverload(candidates, { clsName + "@" }, table);
    CHECK(matchHandle.isAmbiguous);

    auto matchRef = ResolveBestOverload(candidates, { clsName }, table);
    CHECK(matchRef.isAmbiguous);
}

TEST_CASE("OverloadResolution - Invariant: Dynamic arity scaling from 0 to 8 arguments")
{
    std::mt19937_64 rng(0x1337BEF9);
    angel_lsp::analysis::OverloadResolver resolver;
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "FuzzFunc");

    for (size_t arity = 0; arity <= 8; ++arity)
    {
        Symbol sym;
        sym.name = fnName;
        sym.type = SymbolType::Function;
        FunctionSignature sig;
        for (size_t p = 0; p < arity; ++p)
        {
            ParameterInformation param;
            param.name = "p" + std::to_string(p);
            param.typeName = angel_lsp::test::GenerateRandomPrimitiveType(rng);
            sig.parameters.push_back(std::move(param));
        }
        sym.signature = sig;
        resolver.addFunction(sym);
    }

    for (size_t arity = 0; arity <= 8; ++arity)
    {
        auto candidates = resolver.findCandidates(fnName, arity);
        REQUIRE(candidates.size() == 1);
        CHECK(candidates.front().name == fnName);
        CHECK(candidates.front().GetFunction().parameters.size() == arity);
    }

    // Invalid arity yields zero candidates
    CHECK(resolver.findCandidates(fnName, 99).empty());
}

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

TEST_CASE("Overload resolution - PERF-01 Arity Hash Bucket Indexing Invariant")
{
    angel_lsp::analysis::OverloadResolver resolver;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("func");

    std::vector<Symbol> symbols;
    for (size_t arity = 0; arity < 5; ++arity)
    {
        Symbol sym;
        sym.name = fnName;
        sym.type = SymbolType::Function;
        FunctionSignature sig;
        for (size_t p = 0; p < arity; ++p)
        {
            ParameterInformation param;
            param.name = "p" + std::to_string(p);
            param.typeName = "int";
            sig.parameters.push_back(std::move(param));
        }
        sym.signature = sig;
        symbols.push_back(sym);
        resolver.addFunction(sym);
    }

    for (size_t arity = 0; arity < 5; ++arity)
    {
        auto candidates = resolver.findCandidates(fnName, arity);
        REQUIRE(candidates.size() == 1);
        CHECK(candidates.front().name == fnName);
        CHECK(candidates.front().GetFunction().parameters.size() == arity);
    }

    // Non-existent arity
    auto noCandidates = resolver.findCandidates(fnName, 99);
    CHECK(noCandidates.empty());

    // Non-existent name
    auto wrongName = resolver.findCandidates("NonExistent", 0);
    CHECK(wrongName.empty());
}
