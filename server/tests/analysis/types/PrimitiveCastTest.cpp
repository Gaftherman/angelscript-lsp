/**
 * @file PrimitiveCastTest.cpp
 * @brief Unit tests for constructor-style primitive casting and contextual keyword usage.
 */

#include "analysis/SemanticAnalyzer.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_SUITE("analysis/types/PrimitiveCast")
{
    /**
     * @brief Verifies that constructor-style primitive casting like int(uint8) resolves type correctly.
     */
    TEST_CASE("PrimitiveCast - Resolves constructor-style primitive casts")
    {
        const std::string funcName = test::GenerateRandomSymbolName("CastFunc");
        const std::string code = "void " + funcName + "(uint8 val)\n" + "{\n" + "    int(val);\n" + "}\n";

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;

        const std::string fileUri = "file:///prim_cast.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        TSNode callNode{};
        auto search = [&callNode](TSNode node, auto& self) -> void
        {
            const std::string_view nodeType(ts_node_type(node));
            if (nodeType == "functional_cast_expression" || nodeType == "call_expression")
            {
                callNode = node;
                return;
            }
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                self(ts_node_named_child(node, i), self);
                if (!ts_node_is_null(callNode))
                    return;
            }
        };
        search(root, search);

        REQUIRE(!ts_node_is_null(callNode));

        const TSPoint pt = ts_node_start_point(callNode);
        const Scope* scope = FindInnermostScope(scopeRoot.get(), pt.row, pt.column);

        NodeIndex index(root);
        ExpressionTypeContext ctx{scope, table, code, fileUri, &index};
        const IndexedByteRange callBytes{ts_node_start_byte(callNode), ts_node_end_byte(callNode)};
        std::string exprType = ResolveExpressionType(callBytes, ctx);

        CHECK(exprType == "int");

        ts_tree_delete(tree);
    }

    /**
     * @brief Verifies direct initialization where variable name matches its class name.
     */
    TEST_CASE("PrimitiveCast - Direct initialization when variable name matches class name")
    {
        const std::string className = test::GenerateRandomSymbolName("Logger");

        const std::string code = "class " + className + "\n" + "{\n" + "    " + className + "(int priority) {}\n" +
                                 "}\n" + className + " global" + className + "(10);\n" + "void Test()\n" + "{\n" +
                                 "    " + className + " " + className + "(5);\n" + "}\n";

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static i18n::I18n i18n;

        const std::string fileUri = "file:///logger_init.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
        request.scopeRoot = std::move(scopeRoot);
        request.sourceCode = code;
        NodeIndex nodeIndex(ts_tree_root_node(tree));
        request.nodeIndex = &nodeIndex;

        SemanticAnalyzer analyzer(nullptr);
        auto diags = analyzer.Analyze(request);

        // Should not have any "no matching constructor" errors
        for (const auto& d : diags)
        {
            CHECK(d.code != "as-err-no-matching-constructor");
            CHECK(d.code != "as-err-call-argument-count");
        }

        // Verify that CallReference was emitted for the constructor call inside Test()
        auto callRefs = table.FindSymbols(className);
        bool hasCallRef = false;
        for (const auto& sym : callRefs)
        {
            if (sym.type == SymbolType::CallReference)
            {
                hasCallRef = true;
                break;
            }
        }
        CHECK(hasCallRef);

        ts_tree_delete(tree);
    }

    /**
     * @brief Verifies that contextual keywords like 'function' can be used as variables and in comparisons.
     */
    TEST_CASE("PrimitiveCast - Contextual keyword 'function' in handle comparison")
    {
        const std::string cbName = test::GenerateRandomSymbolName("Callback");

        const std::string code = "funcdef void " + cbName + "();\n" + "void Process(" + cbName + "@ function)\n" +
                                 "{\n" + "    if (function !is null)\n" + "    {\n" + "    }\n" + "}\n";

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static i18n::I18n i18n;

        const std::string fileUri = "file:///contextual_func.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
        request.scopeRoot = std::move(scopeRoot);
        request.sourceCode = code;
        NodeIndex nodeIndex(ts_tree_root_node(tree));
        request.nodeIndex = &nodeIndex;

        SemanticAnalyzer analyzer(nullptr);
        auto diags = analyzer.Analyze(request);

        // Zero errors expected
        for (const auto& d : diags)
        {
            CHECK(d.severity != DiagnosticSeverity::Error);
        }

        ts_tree_delete(tree);
    }

    static std::vector<Diagnostic> AnalyzeTypeConversionSnippet(const std::string& code)
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static i18n::I18n i18n;

        const std::string fileUri = "file:///type_conv_test.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
        request.scopeRoot = std::move(scopeRoot);
        request.sourceCode = code;
        NodeIndex nodeIndex(root);
        request.nodeIndex = &nodeIndex;

        SemanticAnalyzer analyzer(nullptr);
        auto diags = analyzer.Analyze(request);
        ts_tree_delete(tree);
        return diags;
    }

    static bool HasDiagnosticCode(const std::vector<Diagnostic>& diags, std::string_view code)
    {
        for (const auto& d : diags)
        {
            if (d.code == code)
            {
                return true;
            }
        }
        return false;
    }

    TEST_CASE("PrimitiveCast - Unrelated class handle initialization is rejected")
    {
        const std::string clsA = test::GenerateRandomSymbolName("TypeA");
        const std::string clsB = test::GenerateRandomSymbolName("TypeB");
        const std::string varA = test::GenerateRandomSymbolName("a");
        const std::string varB = test::GenerateRandomSymbolName("b");

        const std::string goodCode = "class " + clsA + " {}\nclass " + clsB + " : " + clsA + " {}\nvoid main() { " +
                                     clsB + " " + varB + "; " + clsA + "@ " + varA + " = @" + varB + "; }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(goodCode), "as-err-no-implicit-conversion"));

        const std::string normalCode = "class " + clsA + " {}\nvoid main() { " + clsA + " " + varA + "; " + clsA +
                                       "@ " + varB + " = @" + varA + "; }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(normalCode), "as-err-no-implicit-conversion"));

        const std::string badCode = "class " + clsA + " {}\nclass " + clsB + " {}\nvoid main() { " + clsA + " " + varA +
                                    "; " + clsB + "@ " + varB + " = @" + varA + "; }\n";
        CHECK(HasDiagnosticCode(AnalyzeTypeConversionSnippet(badCode), "as-err-no-implicit-conversion"));
    }

    TEST_CASE("PrimitiveCast - Funcdef handle initialized from non-function object handle is rejected")
    {
        const std::string fdName = test::GenerateRandomSymbolName("Callback");
        const std::string clsName = test::GenerateRandomSymbolName("Dummy");
        const std::string fnName = test::GenerateRandomSymbolName("myFn");
        const std::string varName = test::GenerateRandomSymbolName("cb");

        const std::string goodCode = "funcdef void " + fdName + "();\nvoid " + fnName + "() {}\nvoid main() { " +
                                     fdName + "@ " + varName + " = @" + fnName + "; }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(goodCode), "as-err-no-implicit-conversion"));

        const std::string normalCode =
            "funcdef void " + fdName + "();\nvoid main() { " + fdName + "@ " + varName + " = null; }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(normalCode), "as-err-no-implicit-conversion"));

        const std::string badCode = "funcdef void " + fdName + "();\nclass " + clsName + " {}\nvoid main() { " +
                                    clsName + " d; " + fdName + "@ " + varName + " = @d; }\n";
        CHECK(HasDiagnosticCode(AnalyzeTypeConversionSnippet(badCode), "as-err-no-implicit-conversion"));
    }

    TEST_CASE("PrimitiveCast - Delegate construction requires matching method signature")
    {
        const std::string fdName = test::GenerateRandomSymbolName("Action");
        const std::string clsName = test::GenerateRandomSymbolName("Worker");
        const std::string methodName = test::GenerateRandomSymbolName("run");

        const std::string goodCode = "funcdef void " + fdName + "(int);\nclass " + clsName + " { void " + methodName +
                                     "(int x) {} }\nvoid main() { " + clsName + " w; " + fdName + "@ act = " + fdName +
                                     "(w." + methodName + "); }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(goodCode), "as-err-cannot-create-delegate"));

        const std::string normalCode = "funcdef void " + fdName + "();\nclass " + clsName + " { void " + methodName +
                                       "() {} void " + methodName + "(int x) {} }\nvoid main() { " + clsName + " w; " +
                                       fdName + "@ act = " + fdName + "(w." + methodName + "); }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(normalCode), "as-err-cannot-create-delegate"));

        const std::string badCode = "funcdef void " + fdName + "();\nclass " + clsName + " { void " + methodName +
                                    "(int x) {} }\nvoid main() { " + clsName + " w; " + fdName + "@ act = " + fdName +
                                    "(w." + methodName + "); }\n";
        CHECK(HasDiagnosticCode(AnalyzeTypeConversionSnippet(badCode), "as-err-cannot-create-delegate"));
    }

    TEST_CASE("PrimitiveCast - Prefix unary operators require zero-parameter opNeg or opCom")
    {
        const std::string clsName = test::GenerateRandomSymbolName("UnaryBox");
        const std::string varName = test::GenerateRandomSymbolName("b");

        const std::string goodCode = "class " + clsName + " { int opNeg() const { return -1; } }\nvoid main() { " +
                                     clsName + " " + varName + "; int r = -" + varName + "; }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(goodCode), "as-err-unary-operator-not-found"));

        const std::string normalCode = "class " + clsName + " { int opCom() const { return 0; } }\nvoid main() { " +
                                       clsName + " " + varName + "; int r = ~" + varName + "; }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(normalCode), "as-err-unary-operator-not-found"));

        const std::string badCode = "class " + clsName + " { int opNeg(int x) const { return -x; } }\nvoid main() { " +
                                    clsName + " " + varName + "; int r = -" + varName + "; }\n";
        CHECK(HasDiagnosticCode(AnalyzeTypeConversionSnippet(badCode), "as-err-unary-operator-not-found"));
    }

    TEST_CASE("PrimitiveCast - Functional cast to primitive requires zero-parameter opConv or opImplConv")
    {
        const std::string clsGood = test::GenerateRandomSymbolName("GoodConv");
        const std::string clsNormal = test::GenerateRandomSymbolName("ImplConv");
        const std::string clsBadArity = test::GenerateRandomSymbolName("BadConv");
        const std::string clsNoConv = test::GenerateRandomSymbolName("NoConv");

        const std::string goodCode = "class " + clsGood + " { int opConv() const { return 1; } }\nvoid main() { " +
                                     clsGood + " b; int x = int(b); }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(goodCode), "as-err-no-implicit-conversion"));

        const std::string normalCode = "class " + clsNormal +
                                       " { int opImplConv() const { return 1; } }\nvoid main() { " + clsNormal +
                                       " b; int x = int(b); }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(normalCode), "as-err-no-implicit-conversion"));

        const std::string badArityCode = "class " + clsBadArity +
                                         " { int opConv(int extra) const { return extra; } }\nvoid main() { " +
                                         clsBadArity + " b; int x = int(b); }\n";
        CHECK(HasDiagnosticCode(AnalyzeTypeConversionSnippet(badArityCode), "as-err-no-implicit-conversion"));

        const std::string badNoConvCode =
            "class " + clsNoConv + " {}\nvoid main() { " + clsNoConv + " c; int x = int(c); }\n";
        CHECK(HasDiagnosticCode(AnalyzeTypeConversionSnippet(badNoConvCode), "as-err-no-implicit-conversion"));
    }

    TEST_CASE("PrimitiveCast - Reference cast to scalar primitive is rejected even when opCast is declared")
    {
        const std::string clsA = test::GenerateRandomSymbolName("CastSource");
        const std::string clsB = test::GenerateRandomSymbolName("CastTarget");

        const std::string goodCode = "class " + clsA + " {}\nclass " + clsB + " : " + clsA + " {}\nvoid main() { " +
                                     clsA + "@ a = " + clsB + "(); " + clsB + "@ b = cast<" + clsB + ">(a); }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(goodCode), "as-err-invalid-cast"));

        const std::string normalCode = "class " + clsB + " {}\nclass " + clsA + " { " + clsB +
                                       "@ opCast() { return null; } }\nvoid main() { " + clsA + " a; " + clsB +
                                       "@ b = cast<" + clsB + ">(a); }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(normalCode), "as-err-invalid-cast"));

        const std::string badCode = "class " + clsA + " { int opCast() { return 0; } }\nvoid main() { " + clsA +
                                    " b; int x = cast<int>(b); }\n";
        CHECK(HasDiagnosticCode(AnalyzeTypeConversionSnippet(badCode), "as-err-invalid-cast"));
    }

    TEST_CASE("PrimitiveCast - Foreach loop over custom class requires complete opFor iterator set")
    {
        const std::string clsGood = test::GenerateRandomSymbolName("FullCont");
        const std::string clsNormal = test::GenerateRandomSymbolName("FullContZero");
        const std::string clsBad = test::GenerateRandomSymbolName("IncompleteCont");

        const std::string goodCode = "class " + clsGood +
                                     " {\n"
                                     "    int opForBegin() const { return 0; }\n"
                                     "    bool opForEnd(int i) const { return i >= 1; }\n"
                                     "    int opForNext(int i) const { return i + 1; }\n"
                                     "    int opForValue(int i) const { return i; }\n"
                                     "}\nvoid main() { " +
                                     clsGood + " c; foreach (auto v : c) {} }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(goodCode), "as-err-invalid-foreach-container"));

        const std::string normalCode = "class " + clsNormal +
                                       " {\n"
                                       "    int opForBegin() const { return 0; }\n"
                                       "    bool opForEnd(int i) const { return i >= 1; }\n"
                                       "    int opForNext(int i) const { return i + 1; }\n"
                                       "    int opForValue0(int i) const { return i; }\n"
                                       "}\nvoid main() { " +
                                       clsNormal + " c; foreach (auto v : c) {} }\n";
        CHECK_FALSE(HasDiagnosticCode(AnalyzeTypeConversionSnippet(normalCode), "as-err-invalid-foreach-container"));

        const std::string badCode = "class " + clsBad +
                                    " {\n"
                                    "    int opForBegin() const { return 0; }\n"
                                    "    int opForNext(int i) const { return i + 1; }\n"
                                    "    int opForValue(int i) const { return i; }\n"
                                    "}\nvoid main() { " +
                                    clsBad + " c; foreach (auto v : c) {} }\n";
        CHECK(HasDiagnosticCode(AnalyzeTypeConversionSnippet(badCode), "as-err-invalid-foreach-container"));
    }
}
