#include <doctest/doctest.h>
#include "analysis/SemanticHelpers.h"
#include "analysis/OverloadResolver.h"
#include "analysis/DefiniteAssignmentChecker.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
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
    const Scope *FindInnermostScope(const Scope *root, uint32_t line, uint32_t character)
    {
        if (!root) return nullptr;
        const auto contains = [line, character](const Scope &scope ) {
            if (line < scope.startLine || line > scope.endLine) return false;
            if (line == scope.startLine && character < scope.startCharacter) return false;
            if (line == scope.endLine && character > scope.endCharacter) return false;
            return true;
        };
        if (!contains(*root)) return nullptr;
        const Scope *current = root;
        for (bool descended = true; descended;) {
            descended = false;
            for (const auto &child : current->children) {
                if (child && contains(*child)) {
                    current = child.get();
                    descended = true;
                    break;
                }
            }
        }
        return current;
    }

    std::string DeduceTypeAtLastExpression(const std::string &code)
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        const std::string fileUri = "file:///adv_sem_m1.as";
        collector.CollectSymbols(fileUri, code, parser, table);
        auto scopeRoot = scopes.CollectScopes(code, parser);
        TSTree *tree = parser.Parse(code);
        std::string result;
        if (tree) {
            TSNode root = ts_tree_root_node(tree);
            uint32_t count = ts_node_named_child_count(root);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode func = ts_node_named_child(root, i);
                if (std::string_view(ts_node_type(func)) == "func_declaration") {
                    TSNode body = ts_node_child_by_field_name(func, "body", 4);
                    if (!ts_node_is_null(body)) {
                        uint32_t stmtCount = ts_node_named_child_count(body);
                        for (uint32_t j = 0; j < stmtCount; ++j) {
                            TSNode stmt = ts_node_named_child(body, j);
                            std::string_view stmtType = ts_node_type(stmt);
                            if (stmtType == "expression_statement" && ts_node_named_child_count(stmt) > 0) {
                                TSNode expr = ts_node_named_child(stmt, 0);
                                TSPoint pt = ts_node_start_point(expr);
                                const Scope *innerScope = FindInnermostScope(scopeRoot.get(), pt.row, pt.column);
                                result = ResolveExpressionType(expr, innerScope ? innerScope : scopeRoot.get(), table, code, fileUri);
                            }
                            else if (stmtType == "return_statement" && ts_node_named_child_count(stmt) > 0) {
                                TSNode expr = ts_node_named_child(stmt, 0);
                                TSPoint pt = ts_node_start_point(expr);
                                const Scope *innerScope = FindInnermostScope(scopeRoot.get(), pt.row, pt.column);
                                result = ResolveExpressionType(expr, innerScope ? innerScope : scopeRoot.get(), table, code, fileUri);
                            }
                        }
                }
            }
        }
        ts_tree_delete(tree);
    }
    return result;
    }

    std::vector<Diagnostic> AnalyzeCodeForDiagnostics(const std::string &code, const std::string &fileUri = "file:///adv_def_m1.as")
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
        if (request.tree) {
            ts_tree_delete(const_cast<TSTree *>(request.tree));
        }
        return diagnostics;
    }

    bool HasUninitializedRead(const std::vector<Diagnostic> &diagnostics)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                                [](const Diagnostic &d) { return d.code == "as-err-uninitialized-variable-read"; });
    }

    std::vector<Symbol> CollectFunctionCandidates(const std::string &code, const std::string &funcName, SymbolTable &table)
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        const std::string fileUri = "file:///adv_ovl_m1.as";
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

TEST_SUITE("AdversarialSemantics_ExpressionType")
{
    TEST_CASE("Deep 4-Level Method Chain with Mixed Reference Types")
    {
        std::string code =
            "class Level3\n"
            "{\n"
            "    int GetScore() { return 100; }\n"
            "}\n"
            "class Level2\n"
            "{\n"
            "    Level3@ GetLevel3() { return null; }\n"
            "}\n"
            "class Level1\n"
            "{\n"
            "    Level2@ GetLevel2() { return null; }\n"
            "}\n"
            "class Root\n"
            "{\n"
            "    Level1@ GetLevel1() { return null; }\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    Root r;\n"
            "    r.GetLevel1().GetLevel2().GetLevel3().GetScore();\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(code) == "int");
    }

    TEST_CASE("Chained Fluent Builder Methods with Arguments")
    {
        std::string code =
            "class Builder\n"
            "{\n"
            "    Builder@ SetId(int id) { return this; }\n"
            "    Builder@ SetName(string name) { return this; }\n"
            "    double Build() { return 1.0; }\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    Builder b;\n"
            "    b.SetId(10).SetName(\"test\").Build();\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(code) == "double");
    }

    TEST_CASE("Deep 3-Level and 4-Level Generic Template Indexing")
    {
        std::string code3 =
            "void main()\n"
            "{\n"
            "    array<array<dictionary<string, int>>> arr;\n"
            "    arr[0][1][\"key\"];\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(code3) == "int");

        std::string codeDictMap =
            "void main()\n"
            "{\n"
            "    dictionary<string, array<dictionary<int, double>>> d;\n"
            "    d[\"key\"][0][42];\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(codeDictMap) == "double");

        std::string code4 =
            "void main()\n"
            "{\n"
            "    array<array<array<array<float>>>> hyperCube;\n"
            "    hyperCube[0][1][2][3];\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(code4) == "float");
    }

    TEST_CASE("Complex Binary Operators with Mixed Primitives and Exponentiation")
    {
        CHECK(DeduceTypeAtLastExpression("void main() { 2 ** 3; }") == "int");
        CHECK(DeduceTypeAtLastExpression("void main() { 2.0f ** 3; }") == "float");
        CHECK(DeduceTypeAtLastExpression("void main() { 2.0 ** 3; }") == "double");
        CHECK(DeduceTypeAtLastExpression("void main() { int64(10) << 2; }") == "int64");
        CHECK(DeduceTypeAtLastExpression("void main() { uint(5) >> 1; }") == "uint");
        CHECK(DeduceTypeAtLastExpression("void main() { uint64(5) >> 1; }") == "uint64");
        CHECK(DeduceTypeAtLastExpression("void main() { float(1.0f) * double(2.0); }") == "double");
        CHECK(DeduceTypeAtLastExpression("void main() { int64(10) + float(2.0f); }") == "float");
        CHECK(DeduceTypeAtLastExpression("void main() { uint(10) + int64(20); }") == "int64");
    }

    TEST_CASE("Reversed Operator Overloads opSub_r and Inherited opAdd_r")
    {
        std::string subRevCode =
            "class CustomVal\n"
            "{\n"
            "    CustomVal opSub_r(int lhs) const { return this; }\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    CustomVal cv;\n"
            "    100 - cv;\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(subRevCode) == "CustomVal");

        std::string inheritedRevCode =
            "class BaseVal\n"
            "{\n"
            "    BaseVal opAdd_r(double lhs) const { return this; }\n"
            "}\n"
            "class DerivedVal : BaseVal\n"
            "{\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    DerivedVal dv;\n"
            "    3.14 + dv;\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(inheritedRevCode) == "BaseVal");
    }
}

TEST_SUITE("AdversarialSemantics_OverloadResolver")
{
    TEST_CASE("Const vs Non-Const Reference Overloads")
    {
        std::string code =
            "void Modify(int &inout val) { }\n"
            "void Modify(const int &in val) { }\n";
        SymbolTable table;
        auto candidates = CollectFunctionCandidates(code, "Modify", table);
        REQUIRE(candidates.size() == 2);

        auto matchConst = ResolveBestOverload(candidates, { "const int" }, table);
        REQUIRE(matchConst.bestCandidate != nullptr);
        CHECK(matchConst.bestCandidate->GetFunction().parameters[0].isConst == true);

        auto matchNonConst = ResolveBestOverload(candidates, { "int" }, table);
        REQUIRE(matchNonConst.bestCandidate != nullptr);
        CHECK(matchNonConst.bestCandidate->GetFunction().parameters[0].isConst == false);
    }

    TEST_CASE("Tricky Overload Ambiguities with Multiple Arguments")
    {
        std::string code =
            "void Dispatch(int a, float b) { }\n"
            "void Dispatch(float a, int b) { }\n";
        SymbolTable table;
        auto candidates = CollectFunctionCandidates(code, "Dispatch", table);
        REQUIRE(candidates.size() == 2);

        auto res = ResolveBestOverload(candidates, { "int", "int" }, table);
        CHECK(res.isAmbiguous);
    }

    TEST_CASE("Default Parameter Penalty with Numeric Widening Hierarchy")
    {
        std::string code =
            "int Calc(int a, int b = 0, int c = 0) { return 0; }\n"
            "double Calc(double a, int b = 0) { return 0.0; }\n"
            "float Calc(float a) { return 0.0f; }\n";
        SymbolTable table;
        auto candidates = CollectFunctionCandidates(code, "Calc", table);
        REQUIRE(candidates.size() == 3);

        auto matchInt = ResolveBestOverload(candidates, { "int" }, table);
        REQUIRE(matchInt.bestCandidate != nullptr);
        CHECK(matchInt.bestCandidate->GetFunction().returnType == "int");

        auto matchFloat = ResolveBestOverload(candidates, { "float" }, table);
        REQUIRE(matchFloat.bestCandidate != nullptr);
        CHECK(matchFloat.bestCandidate->GetFunction().returnType == "float");
    }
}

TEST_SUITE("AdversarialSemantics_DefiniteAssignment")
{
    TEST_CASE("Nested Loops and Complex Break Dataflow")
    {
        std::string nestedWhile =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool cond = false;\n"
            "    while (true)\n"
            "    {\n"
            "        while (cond)\n"
            "        {\n"
            "            x = 10;\n"
            "        }\n"
            "        break;\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(nestedWhile)));

        std::string nestedDoWhile =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    do\n"
            "    {\n"
            "        do\n"
            "        {\n"
            "            x = 42;\n"
            "        } while (false);\n"
            "    } while (false);\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCodeForDiagnostics(nestedDoWhile)));
    }

    TEST_CASE("Switch Statements With Partial Assignment or Missing Default")
    {
        std::string switchNoDefault =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    int val = 2;\n"
            "    switch (val)\n"
            "    {\n"
            "    case 1: x = 10; break;\n"
            "    case 2: x = 20; break;\n"
            "    case 3: x = 30; break;\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(switchNoDefault)));

        std::string switchPartial =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    int val = 2;\n"
            "    switch (val)\n"
            "    {\n"
            "    case 1: x = 10; break;\n"
            "    case 2: break;\n"
            "    default: x = 30; break;\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(switchPartial)));
    }
}
