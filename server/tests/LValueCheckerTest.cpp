#include <doctest/doctest.h>
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include <algorithm>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    std::vector<Diagnostic> RunAnalysis(const std::string &code)
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector(nullptr);
        LocalScopeCollector scopeCollector(nullptr);
        SymbolTable symbolTable;

        std::string uri = "file:///test.as";
        TSTree *tree = parser.Parse(code);
        symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
        auto scopeRoot = scopeCollector.CollectScopes(code, parser);

        SemanticAnalysisRequest req{symbolTable, uri};
        req.sourceCode = code;
        req.tree = tree;
        req.scopeRoot = std::move(scopeRoot);

        SemanticAnalyzer analyzer(nullptr);
        auto diagnostics = analyzer.Analyze(req);

        if (tree)
        {
            ts_tree_delete(tree);
        }

        return diagnostics;
    }
}

TEST_CASE("LValueChecker - Local variable cannot be of type void")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    void v;\n"
        "    v = 0;\n"
        "}\n";

    auto diags = RunAnalysis(code);
    bool hasVoidVarError = std::any_of(diags.begin(), diags.end(),
        [](const Diagnostic &d) { return d.code == "as-err-void-variable"; });
    CHECK(hasVoidVarError);
}

TEST_CASE("LValueChecker - Cannot assign to void-returning method call")
{
    std::string code =
        "class AnotherClass\n"
        "{\n"
        "    void AnotherMethod(float f)\n"
        "    {\n"
        "    }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    AnotherClass myClass;\n"
        "    myClass.AnotherMethod(2.0f) = null;\n"
        "}\n";

    auto diags = RunAnalysis(code);
    bool hasAssignVoidError = std::any_of(diags.begin(), diags.end(),
        [](const Diagnostic &d) { return d.code == "as-err-assign-void" || d.code == "as-err-not-lvalue"; });
    CHECK(hasAssignVoidError);
}

TEST_CASE("LValueChecker - Cannot assign to value-returning namespace function call")
{
    std::string code =
        "namespace MyNamespace\n"
        "{\n"
        "    float MyNamespaceFunction()\n"
        "    {\n"
        "        return 0.0f;\n"
        "    }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    MyNamespace::MyNamespaceFunction() = \"hola\";\n"
        "}\n";

    auto diags = RunAnalysis(code);
    bool hasAssignNonRefError = std::any_of(diags.begin(), diags.end(),
        [](const Diagnostic &d) { return d.code == "as-err-assign-non-ref-call" || d.code == "as-err-not-lvalue"; });
    CHECK(hasAssignNonRefError);
}

TEST_CASE("LValueChecker - Assigning to reference-returning function is valid")
{
    std::string code =
        "int g_val = 0;\n"
        "int& GetRef()\n"
        "{\n"
        "    return g_val;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    GetRef() = 42;\n"
        "}\n";

    auto diags = RunAnalysis(code);
    bool hasLValueError = std::any_of(diags.begin(), diags.end(),
        [](const Diagnostic &d) {
            return d.code == "as-err-assign-void" ||
                   d.code == "as-err-assign-non-ref-call" ||
                   d.code == "as-err-not-lvalue";
        });
    CHECK_FALSE(hasLValueError);
}

TEST_CASE("LValueChecker - Cannot assign to literal")
{
    std::string code =
        "void main()\n"
        "{\n"
        "    123 = 456;\n"
        "}\n";

    auto diags = RunAnalysis(code);
    bool hasNotLValueError = std::any_of(diags.begin(), diags.end(),
        [](const Diagnostic &d) { return d.code == "as-err-not-lvalue"; });
    CHECK(hasNotLValueError);
}
