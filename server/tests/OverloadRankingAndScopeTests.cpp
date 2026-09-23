/**
 * @file OverloadRankingAndScopeTests.cpp
 * @brief Invariant-based tests for formal conversion ranking, component-wise partial ordering,
 *        cyclic auto false-positive elimination, and hover scope cycle defense.
 */

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/OverloadResolver.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "features/hover/HoverHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::test;

namespace
{
/**
 * @brief Helper to create a candidate function symbol with specified parameters.
 * @param[in] name Function identifier.
 * @param[in] returnType Return type string.
 * @param[in] params Vector of parameter type and parameter name pairs.
 * @return Constructed Symbol object.
 */
Symbol CreateFunctionCandidate(const std::string& name, const std::string& returnType,
                               const std::vector<std::pair<std::string, std::string>>& params)
{
    Symbol sym;
    sym.name = name;
    sym.type = SymbolType::Function;
    FunctionSignature sig;
    sig.returnType = returnType;
    for (const auto& [typeName, paramName] : params)
    {
        ParameterInformation param;
        param.typeName = typeName;
        param.name = paramName;
        sig.parameters.push_back(std::move(param));
    }
    sym.signature = sig;
    return sym;
}

/**
 * @brief Analyzes a script snippet and returns collected diagnostics.
 * @param[in] code AngelScript source text.
 * @return Vector of emitted diagnostics.
 */
std::vector<Diagnostic> AnalyzeSnippet(const std::string& code)
{
    parser::AngelScriptParser parser;
    SymbolTable table;
    SymbolCollector collector(nullptr);
    auto diags = collector.CollectSymbols("file:///test.as", code, parser, table);

    LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(code, parser);

    TSTree* tree = parser.Parse(code);

    SemanticAnalyzer analyzer(nullptr);
    SemanticAnalysisRequest request{table, "file:///test.as", ".as.predefined", nullptr};
    request.sourceCode = code;
    request.tree = tree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());
    if (tree)
    {
        ts_tree_delete(tree);
    }
    return diags;
}

/**
 * @brief Checks if any diagnostic matches the given code.
 * @param[in] diags List of diagnostics.
 * @param[in] code Diagnostic code string view.
 * @return True if matched.
 */
bool HasDiagCode(const std::vector<Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(), [code](const Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_SUITE("OverloadRankingAndScope")
{
    TEST_CASE("Container specialization selects exact container type over alternative instantiations")
    {
        const std::string fnName = GenerateRandomSymbolName("ToArray");
        SymbolTable table;

        Symbol candInt = CreateFunctionCandidate(fnName, "void", {{"array<int>& out", "outArr"}});
        Symbol candStr = CreateFunctionCandidate(fnName, "void", {{"array<string>& out", "outArr"}});
        const std::vector<Symbol> candidates = {candInt, candStr};

        // Call with array<int> (lvalue)
        auto matchInt = ResolveBestOverload(candidates, {"array<int>"}, table, {true});
        REQUIRE(matchInt.bestCandidate != nullptr);
        CHECK_FALSE(matchInt.isAmbiguous);
        CHECK(matchInt.bestCandidate->GetFunction().parameters[0].typeName == "array<int>& out");

        // Call with array<string> (lvalue)
        auto matchStr = ResolveBestOverload(candidates, {"array<string>"}, table, {true});
        REQUIRE(matchStr.bestCandidate != nullptr);
        CHECK_FALSE(matchStr.isAmbiguous);
        CHECK(matchStr.bestCandidate->GetFunction().parameters[0].typeName == "array<string>& out");

        // Call with array<float>: incompatible with both instantiations
        auto matchFloat = ResolveBestOverload(candidates, {"array<float>"}, table, {true});
        CHECK(matchFloat.bestCandidate == nullptr);
        CHECK(matchFloat.viableCandidates.empty());
    }

    TEST_CASE("Pairwise partial ordering marks unorderable candidates as ambiguous with null bestCandidate")
    {
        const std::string fnName = GenerateRandomSymbolName("Dispatch");
        SymbolTable table;

        // foo(int, double) vs foo(double, int)
        Symbol cand1 = CreateFunctionCandidate(fnName, "void", {{"int", "a"}, {"double", "b"}});
        Symbol cand2 = CreateFunctionCandidate(fnName, "void", {{"double", "a"}, {"int", "b"}});
        const std::vector<Symbol> candidates = {cand1, cand2};

        // Called with (int, int):
        // Candidate 1 has (Exact, StandardConv)
        // Candidate 2 has (StandardConv, Exact)
        // Neither dominates: must be ambiguous and bestCandidate MUST be nullptr
        auto match = ResolveBestOverload(candidates, {"int", "int"}, table, {false, false});
        CHECK(match.isAmbiguous);
        CHECK(match.bestCandidate == nullptr);
        CHECK(match.viableCandidates.size() == 2);
    }

    TEST_CASE("Pairwise partial ordering produces ambiguity diagnostic in end-to-end call checking")
    {
        const std::string fnName = GenerateRandomSymbolName("OrderAmbiguous");
        const std::string script = "void " + fnName + "(int a, double b) {}\n" + "void " + fnName +
                                   "(double a, int b) {}\n" + "void Caller() {\n" + "    " + fnName + "(10, 20);\n" +
                                   "}\n";

        auto diags = AnalyzeSnippet(script);
        CHECK(HasDiagCode(diags, "as-err-call-ambiguous"));
    }

    TEST_CASE("Cyclic auto resolution eliminates false positives on member access, string literals, and comments")
    {
        const std::string clsName = GenerateRandomSymbolName("PlayerMove");
        const std::string propPlayer = GenerateRandomSymbolName("player");
        const std::string propOwner = GenerateRandomSymbolName("owner");
        const std::string fnGetMode = GenerateRandomSymbolName("GetMode");
        const std::string fnGetSpeed = GenerateRandomSymbolName("GetSpeed");
        const std::string helperVar = GenerateRandomSymbolName("pmove");
        const std::string autoPlayer = GenerateRandomSymbolName("player");
        const std::string autoOwner = GenerateRandomSymbolName("owner");
        const std::string autoPred = GenerateRandomSymbolName("client_prediction");
        const std::string autoSpeed = GenerateRandomSymbolName("speed");

        std::string validSnippet = "class " + clsName +
                                   " {\n"
                                   "    int " +
                                   propPlayer +
                                   ";\n"
                                   "    int " +
                                   propOwner +
                                   ";\n"
                                   "}\n"
                                   "string " +
                                   fnGetMode +
                                   "(const string& in name) { return name; }\n"
                                   "int " +
                                   fnGetSpeed +
                                   "(int val) { return val; }\n"
                                   "void TestValid(" +
                                   clsName + "@ " + helperVar +
                                   ") {\n"
                                   "    auto " +
                                   autoPlayer + " = " + helperVar + "." + propPlayer +
                                   ";\n"
                                   "    auto " +
                                   autoOwner + " = " + helperVar + "." + propOwner +
                                   ";\n"
                                   "    auto " +
                                   autoPred + " = " + fnGetMode + "(\"" + autoPred +
                                   "\");\n"
                                   "    auto " +
                                   autoSpeed + " = " + fnGetSpeed + "(42 /* " + autoSpeed +
                                   " */);\n"
                                   "}\n";

        auto validDiags = AnalyzeSnippet(validSnippet);
        CHECK_FALSE(HasDiagCode(validDiags, "as-err-cyclic-auto-dependency"));

        // Verify true cyclic dependency is still caught
        const std::string cyclicVar = GenerateRandomSymbolName("cyclic");
        std::string cyclicSnippet = "void TestCyclic() {\n"
                                    "    auto " +
                                    cyclicVar + " = " + cyclicVar +
                                    ";\n"
                                    "}\n";

        auto cyclicDiags = AnalyzeSnippet(cyclicSnippet);
        CHECK(HasDiagCode(cyclicDiags, "as-err-cyclic-auto-dependency"));
    }

    TEST_CASE("Scope parent cycle defense terminates safely without recursion or hang")
    {
        Scope s1;
        s1.startLine = 0;
        s1.endLine = 10;
        Scope s2;
        s2.startLine = 0;
        s2.endLine = 10;

        // Form an intentional circular reference
        s1.parent = &s2;
        s2.parent = &s1;

        const LocalDefinition* def = ResolveInScope(&s1, "non_existent_symbol");
        CHECK(def == nullptr);
    }
}
