#include <doctest/doctest.h>

#include "analysis/ASTUtils.h"
#include "analysis/CallChecker.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <random>
#include <string>
#include <vector>

namespace
{
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

/**
 * @brief Constructs a randomized AngelScript script with classes and functions.
 * @param[in] rng Pseudorandom generator.
 * @param[in] functionCount Number of methods to synthesize.
 * @return Synthesized script source.
 */
std::string BuildRandomizedScript(std::mt19937_64& rng, size_t functionCount)
{
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("Cls");
    std::string code;
    code.reserve(functionCount * 120 + 256);
    code += "class " + className + "\n{\n";
    code += "    int m_val;\n";

    for (size_t i = 0; i < functionCount; ++i)
    {
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Func");
        const int constant = static_cast<int>(rng() % 1000);
        code += "    void " + fnName + "(int p" + std::to_string(i) + ")\n    {\n";
        code += "        m_val = p" + std::to_string(i) + " + " + std::to_string(constant) + ";\n";
        code += "    }\n\n";
    }
    code += "}\n";
    return code;
}

} // namespace

TEST_CASE("Iteration Budget - Linear AST Traversal Invariant on Script Analysis")
{
    std::mt19937_64 rng(42);
    const std::string sourceCode = BuildRandomizedScript(rng, 25);

    AngelScriptParser parser;
    TSTree* tree = parser.Parse(sourceCode);
    REQUIRE(tree != nullptr);

    const TSNode root = ts_tree_root_node(tree);
    const size_t namedCount = CountNamedNodes(root);
    CHECK(namedCount > 0);

    TraversalBudget budget(root, 3.0);
    CHECK(budget.GetNamedNodeCount() == namedCount);
    CHECK(budget.GetMaxAllowed() == static_cast<size_t>(static_cast<double>(namedCount) * 3.0));
    CHECK(budget.GetCurrentVisits() == 0);

    SymbolTable symbolTable;
    const std::string uri = "file:///" + angel_lsp::test::GenerateRandomSymbolName("test") + ".as";
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(uri, sourceCode, parser, symbolTable);

    SemanticAnalysisRequest request(symbolTable, uri);
    request.tree = tree;
    request.sourceCode = sourceCode;
    request.traversalBudget = &budget;

    SemanticAnalyzer analyzer(nullptr);
    const auto diagnostics = analyzer.Analyze(request);

    // Verify linear O(N) traversal budget invariant: visits <= 3 * M_nodes
    const size_t currentVisits = budget.GetCurrentVisits();
    CHECK(currentVisits > 0);
    CHECK(currentVisits <= budget.GetMaxAllowed());
    CHECK_FALSE(budget.IsExceeded());

    ts_tree_delete(tree);
}

TEST_CASE("Iteration Budget - Pathological Traversal Throws BudgetExceededException Invariant")
{
    std::mt19937_64 rng(1337);
    const std::string sourceCode = BuildRandomizedScript(rng, 5);

    AngelScriptParser parser;
    TSTree* tree = parser.Parse(sourceCode);
    REQUIRE(tree != nullptr);

    const TSNode root = ts_tree_root_node(tree);
    const size_t namedCount = CountNamedNodes(root);
    REQUIRE(namedCount > 0);

    TraversalBudget tightBudget(namedCount, 1.0);
    CHECK(tightBudget.GetMaxAllowed() == namedCount);

    // Record visits up to the limit
    tightBudget.RecordVisit(namedCount);
    CHECK(tightBudget.GetCurrentVisits() == namedCount);
    CHECK_FALSE(tightBudget.IsExceeded());

    // Exceeding limit must throw BudgetExceededException
    CHECK_THROWS_AS(tightBudget.RecordVisit(1), BudgetExceededException);

    try
    {
        tightBudget.RecordVisit(1);
    }
    catch (const BudgetExceededException& ex)
    {
        CHECK(ex.GetVisits() > ex.GetMaxAllowed());
        CHECK(ex.GetMaxAllowed() == namedCount);
    }

    ts_tree_delete(tree);
}

TEST_CASE("Iteration Budget - Multi-Scale Linear O(N) Scaling Invariant")
{
    std::mt19937_64 rng(999);
    AngelScriptParser parser;

    const std::vector<size_t> scales = {10, 30, 60};
    for (size_t scale : scales)
    {
        const std::string sourceCode = BuildRandomizedScript(rng, scale);
        TSTree* tree = parser.Parse(sourceCode);
        REQUIRE(tree != nullptr);

        const TSNode root = ts_tree_root_node(tree);
        const size_t namedCount = CountNamedNodes(root);

        TraversalBudget budget(root, 3.0);
        NodeIndex index(root, nullptr, &budget);

        const size_t visits = budget.GetCurrentVisits();
        const double ratio = static_cast<double>(visits) / static_cast<double>(namedCount);

        // Preorder indexation visits nodes in linear time
        CHECK(visits > 0);
        CHECK(ratio < 3.0);
        CHECK_FALSE(budget.IsExceeded());

        ts_tree_delete(tree);
    }
}

TEST_CASE("Iteration Budget - CallChecker Candidate Arity Budget and Memory Inspection")
{
    // Construct test candidate functions for live memory inspection in DAP
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("TargetCall");
    std::vector<Symbol> candidates;
    candidates.reserve(4);

    for (uint32_t arity = 1; arity <= 3; ++arity)
    {
        FunctionSignature sig;
        sig.returnType = "void";
        for (uint32_t p = 0; p < arity; ++p)
        {
            ParameterInformation param;
            param.name = "arg" + std::to_string(p);
            param.typeName = "int";
            sig.parameters.push_back(std::move(param));
        }

        Symbol sym;
        sym.name = fnName;
        sym.type = SymbolType::Function;
        sym.signature = std::move(sig);
        candidates.push_back(std::move(sym));
    }

    const uint32_t targetArity = 2;
    const std::vector<const Symbol*> matching = FilterMatchingArityCandidates(candidates, targetArity);

    CHECK(candidates.size() == 3);
    CHECK(matching.size() == 1);

    TraversalBudget callBudget(10, 3.0);
    callBudget.RecordVisit(matching.size());
    const size_t currentVisits = callBudget.GetCurrentVisits();
    CHECK(currentVisits == 1);
}
