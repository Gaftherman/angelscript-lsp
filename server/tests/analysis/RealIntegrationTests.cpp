#include <doctest/doctest.h>
#include "analysis/PredefinedLoader.h"
#include "analysis/ValidationOracle.h"
#include "analysis/SymbolTable.h"
#include "document/Document.h"
#include "lsp/types.h"

TEST_SUITE("RealIntegrationTests")
{
    TEST_CASE("Real as.predefined Parsing")
    {
        analysis::SymbolTable globalTable;
        std::string mockPredefined = R"(
            typedef uint32 ConCommandFlags_t;
            funcdef bool less(const ?&in a, const ?&in b);
            class CBasePlayer {
                void Kill();
                entvars_t@ pev;
            }
        )";

        analysis::PredefinedLoader::LoadFromSource(mockPredefined, globalTable, "string", "array");

        const analysis::Symbol *typedefSym = globalTable.FindGlobalByName("ConCommandFlags_t");
        REQUIRE(typedefSym != nullptr);
        CHECK(typedefSym->name == "ConCommandFlags_t");
        CHECK(typedefSym->kind == analysis::SymbolKind::Typedef);

        const analysis::Symbol *funcdefSym = globalTable.FindGlobalByName("less");
        REQUIRE(funcdefSym != nullptr);
        CHECK(funcdefSym->name == "less");
        CHECK(funcdefSym->kind == analysis::SymbolKind::Funcdef);

        const analysis::Symbol *classSym = globalTable.FindGlobalByName("CBasePlayer");
        REQUIRE(classSym != nullptr);
        CHECK(classSym->name == "CBasePlayer");
        CHECK(classSym->kind == analysis::SymbolKind::Class);

        const analysis::Symbol *methodSym = globalTable.FindByNameDeep("CBasePlayer::Kill");
        if (!methodSym && classSym)
        {
            for (const auto &child : classSym->children)
            {
                if (child->name == "Kill")
                {
                    methodSym = child.get();
                    break;
                }
            }
        }
        REQUIRE(methodSym != nullptr);
        CHECK(methodSym->name == "Kill");
        CHECK((methodSym->kind == analysis::SymbolKind::Method || methodSym->kind == analysis::SymbolKind::Function));
    }

    TEST_CASE("Tree-Sitter Syntax Diagnostic Catching")
    {
        analysis::ValidationOracle oracle;
        std::string brokenSyntaxCode = R"(
            void Main() {
                int a = ;
            }
        )";

        auto diags = oracle.ValidateSync(brokenSyntaxCode, "file:///test_syntax.as");
        REQUIRE(!diags.empty());

        bool foundSyntaxErr = false;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                foundSyntaxErr = true;
                break;
            }
        }
        CHECK(foundSyntaxErr);
    }

    TEST_CASE("Tree-Sitter Semantic Diagnostic Catching")
    {
        analysis::ValidationOracle oracle;
        std::string invalidSemanticCode = R"(
            void Main() {
                UnknownFunctionCall();
            }
        )";

        auto diags = oracle.ValidateSync(invalidSemanticCode, "file:///test_semantic.as");
        REQUIRE(!diags.empty());

        bool foundSemanticErr = false;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error && d.message.find("UnknownFunctionCall") != std::string::npos)
            {
                foundSemanticErr = true;
                break;
            }
        }
        CHECK(foundSemanticErr);
    }

    TEST_CASE("Uncalled Standalone Method Call Error Catching")
    {
        analysis::SymbolTable globalTable;
        std::string mockPredefined = R"(
            class array {
                void insertLast(int val);
            }
        )";
        analysis::PredefinedLoader::LoadFromSource(mockPredefined, globalTable, "string", "array");

        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                array numbers;
                numbers.insertLast;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_uncalled_method.as", nullptr, &globalTable);
        REQUIRE(!diags.empty());

        bool foundUncalledErr = false;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error && d.message.find("insertLast") != std::string::npos)
            {
                foundUncalledErr = true;
                break;
            }
        }
        CHECK(foundUncalledErr);
    }

    TEST_CASE("Duplicate Local Variable Scope Redefinition Catching")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                int f = 1;
                float f = 2;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_duplicate_var.as");
        REQUIRE(!diags.empty());

        bool foundRedefinitionErr = false;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error && (d.message.find("Redefinition") != std::string::npos || d.message.find("Redefinición") != std::string::npos))
            {
                foundRedefinitionErr = true;
                break;
            }
        }
        CHECK(foundRedefinitionErr);
    }
}

TEST_SUITE("Type Inference and Mismatch")
{
    TEST_CASE("Test A: int f = true; -> Assert exactly 1 diagnostic error")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                int f = true;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_a.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 1);
    }

    TEST_CASE("Test B: float f = 3.14; -> Assert 0 diagnostics")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                float f = 3.14;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_b.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Test C: bool b = false; int c = b; -> Assert exactly 1 diagnostic error on the second statement")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            void Main() {
                bool b = false;
                int c = b;
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_c.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 1);
    }
}

TEST_SUITE("Using and Import Validation")
{
    TEST_CASE("Valid using namespace statement")
    {
        analysis::SymbolTable globalTable;
        std::string mockPredefined = R"(
            namespace Math {
                void Sin() {}
            }
        )";
        analysis::PredefinedLoader::LoadFromSource(mockPredefined, globalTable, "string", "array");

        analysis::ValidationOracle oracle;
        std::string code = "using namespace Math;";

        auto diags = oracle.ValidateSync(code, "file:///test_using_valid.as", nullptr, &globalTable);
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Invalid undeclared using namespace statement")
    {
        analysis::ValidationOracle oracle;
        std::string code = "using namespace NonExistentNamespace;";

        auto diags = oracle.ValidateSync(code, "file:///test_using_invalid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 1);
    }

    TEST_CASE("Invalid import module directive")
    {
        analysis::ValidationOracle oracle;
        std::string code = "import void Kill() from;";

        auto diags = oracle.ValidateSync(code, "file:///test_import_invalid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}

TEST_SUITE("Enum and Typedef Validation")
{
    TEST_CASE("Valid enum with and without explicit values")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            enum Color {
                Red = 1,
                Green = 2,
                Blue
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_enum_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Invalid enum with duplicate constant enumerator")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            enum Color {
                Red = 1,
                Red = 2
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_enum_duplicate.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Invalid enum with non-integer initializer")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            enum State {
                Active = "true"
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_enum_invalid_init.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Valid typedef statement")
    {
        analysis::ValidationOracle oracle;
        std::string code = "typedef float real;";

        auto diags = oracle.ValidateSync(code, "file:///test_typedef_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Invalid typedef with colliding alias name")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            typedef float real;
            typedef int real;
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_typedef_duplicate.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}

TEST_SUITE("Function and Paramlist Validation")
{
    TEST_CASE("Valid function and funcdef with default parameters")
    {
        analysis::ValidationOracle oracle;
        std::string code = R"(
            funcdef void Callback(int a, float b = 1.0f);
            void Main(int a, int b = 2) {}
        )";

        auto diags = oracle.ValidateSync(code, "file:///test_func_valid.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount == 0);
    }

    TEST_CASE("Function with duplicate parameter names")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Foo(int a, float a) {}";

        auto diags = oracle.ValidateSync(code, "file:///test_func_dup_param.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Function with out-of-order default parameter")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Foo(int a = 1, int b) {}";

        auto diags = oracle.ValidateSync(code, "file:///test_func_default_order.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Function returning value in void function")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Foo() { return 42; }";

        auto diags = oracle.ValidateSync(code, "file:///test_func_void_ret.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }

    TEST_CASE("Global function with method-only attribute override")
    {
        analysis::ValidationOracle oracle;
        std::string code = "void Foo() override {}";

        auto diags = oracle.ValidateSync(code, "file:///test_func_invalid_attr.as");
        size_t errorCount = 0;
        for (const auto &d : diags)
        {
            if (d.severity == lsp::DiagnosticSeverity::Error)
            {
                errorCount++;
            }
        }
        CHECK(errorCount >= 1);
    }
}
