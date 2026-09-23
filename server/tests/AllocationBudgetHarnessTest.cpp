#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/OverloadResolver.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "features/hover/HoverHandler.h"
#include "features/signature_help/SignatureHelpHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/AllocationCounter.h"

#include <random>
#include <string>
#include <vector>

namespace
{
using namespace angel_lsp::analysis;
using namespace angel_lsp::features;
using namespace angel_lsp::parser;
using namespace angel_lsp::utils;

struct TestEnvironment
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;

    explicit TestEnvironment(const std::string& code)
        : uri("file:///" + angel_lsp::test::GenerateRandomSymbolName("alloc_doc") + ".as"), sourceCode(code)
    {
        tree = parser.Parse(sourceCode);
        symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
        auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(uri, std::move(rootScope));
        }
    }

    ~TestEnvironment()
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }
};

} // namespace

TEST_CASE("Allocation Budget - Setup Clone Provider")
{
    ScopedAllocationCounter::SetTableCloneProvider(&SymbolTable::GetTableCloneCount);
    SymbolTable::ResetTableCloneCount();
    CHECK(SymbolTable::GetTableCloneCount() == 0);
}

TEST_CASE("Allocation Budget - OverloadResolver Candidate Lookup Churn Invariant")
{
    ScopedAllocationCounter::SetTableCloneProvider(&SymbolTable::GetTableCloneCount);

    OverloadResolver resolver;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Calc");

    std::vector<FunctionSymbol> symbols;
    symbols.reserve(5);
    for (uint32_t arity = 1; arity <= 4; ++arity)
    {
        FunctionSignature sig;
        sig.returnType = "void";
        for (uint32_t p = 0; p < arity; ++p)
        {
            ParameterInformation param;
            param.name = "p" + std::to_string(p);
            param.typeName = "int";
            sig.parameters.push_back(std::move(param));
        }

        Symbol sym;
        sym.name = fnName;
        sym.type = SymbolType::Function;
        sym.signature = std::move(sig);
        symbols.push_back(std::move(sym));
    }

    resolver.indexCandidates(symbols);

    // Warm-up query
    auto warmResult = resolver.findCandidates(fnName, 2);
    REQUIRE(warmResult.size() == 1);

    // Measure allocation churn on warm query
    {
        ScopedAllocationCounter counter;
        auto candidates = resolver.findCandidates(fnName, 2);
        CHECK(candidates.size() == 1);

        // Invariant: <= 5 heap allocations and 0 table clones
        CHECK(counter.GetAllocationCount() <= 5);
        CHECK(counter.GetTableClones() == 0);
    }
}

TEST_CASE("Allocation Budget - Hover Warm Query Allocation Churn Invariant")
{
    ScopedAllocationCounter::SetTableCloneProvider(&SymbolTable::GetTableCloneCount);

    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("speed");
    const std::string script = "void Move()\n{\n    int " + varName + " = 10;\n    " + varName + " = 20;\n}\n";

    TestEnvironment env(script);

    // Find position of varName in assignment
    const size_t pos = env.sourceCode.rfind(varName);
    REQUIRE(pos != std::string::npos);

    uint32_t line = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < pos; ++i)
    {
        if (env.sourceCode[i] == '\n')
        {
            ++line;
            lineStart = i + 1;
        }
    }
    const uint32_t col = static_cast<uint32_t>(pos - lineStart);

    HoverRequest req{env.uri, env.sourceCode, env.tree, env.symbolTable, env.scopeIndex, lsp::Position{line, col}};

    // Warm-up query
    auto warmHover = GetHover(req);
    REQUIRE(warmHover.has_value());

    // Measure allocation churn on warm query
    {
        ScopedAllocationCounter counter;
        auto hover = GetHover(req);
        CHECK(hover.has_value());

        // Invariant: <= 25 heap allocations and 0 table clones
        CHECK(counter.GetAllocationCount() <= 25);
        CHECK(counter.GetTableClones() == 0);
    }
}

TEST_CASE("Allocation Budget - Signature Help Warm Query Allocation Churn Invariant")
{
    ScopedAllocationCounter::SetTableCloneProvider(&SymbolTable::GetTableCloneCount);

    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("FireWeapon");
    const std::string script =
        "void " + fnName + "(int ammo, float speed) {}\n" + "void Test()\n{\n    " + fnName + "(10, 2.5f);\n}\n";

    TestEnvironment env(script);

    // Find position of argument inside call
    const size_t callPos = env.sourceCode.find(fnName + "(10");
    REQUIRE(callPos != std::string::npos);

    const size_t pos = callPos + fnName.length() + 2; // Inside "10"
    uint32_t line = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < pos; ++i)
    {
        if (env.sourceCode[i] == '\n')
        {
            ++line;
            lineStart = i + 1;
        }
    }
    const uint32_t col = static_cast<uint32_t>(pos - lineStart);

    SignatureHelpRequest req{env.uri,         env.sourceCode, env.tree,
                             env.symbolTable, env.scopeIndex, lsp::Position{line, col}};

    // Warm-up query
    auto warmSig = GetSignatureHelp(req);
    REQUIRE(warmSig.has_value());

    // Measure allocation churn on warm query
    {
        ScopedAllocationCounter counter;
        auto sigHelp = GetSignatureHelp(req);
        CHECK(sigHelp.has_value());

        // Invariant: <= 25 heap allocations and 0 table clones
        CHECK(counter.GetAllocationCount() <= 25);
        CHECK(counter.GetTableClones() == 0);
    }
}

TEST_CASE("Allocation Budget - Table Clone Detection Invariant")
{
    ScopedAllocationCounter::SetTableCloneProvider(&SymbolTable::GetTableCloneCount);

    SymbolTable table;
    SymbolTable staging;

    Symbol sym;
    sym.name = angel_lsp::test::GenerateRandomSymbolName("SnapshotSym");
    sym.fileUri = "file:///test.as";
    sym.type = SymbolType::Variable;
    table.AddSymbol(sym);

    ScopedAllocationCounter counter;
    CHECK(counter.GetTableClones() == 0);

    auto snapshot = table.CreateAnalysisSnapshot("file:///test.as", staging);
    CHECK(snapshot != nullptr);

    // Assert that CreateAnalysisSnapshot correctly increments the table clone counter
    CHECK(counter.GetTableClones() == 1);
}
