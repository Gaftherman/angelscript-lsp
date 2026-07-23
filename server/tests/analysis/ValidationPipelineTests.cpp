/**
 * @file ValidationPipelineTests.cpp
 * @brief Unit tests for modular validation pipeline rules and ValidationOracle orchestrator.
 * @ingroup Tests
 */

#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include "analysis/ValidationOracle.h"
#include "analysis/validation/ValidationContext.h"
#include "analysis/validation/rules/SyntaxErrorRule.h"
#include "analysis/validation/rules/TypeValidationRule.h"
#include "analysis/validation/rules/FunctionDeclRule.h"
#include "analysis/validation/rules/ClassDeclRule.h"
#include "analysis/validation/rules/ArgumentRule.h"

using namespace angel_lsp;
using namespace analysis;
using namespace analysis::validation;

TEST_SUITE("ValidationPipeline")
{
    TEST_CASE("SyntaxErrorRule - detects missing or unexpected tokens")
    {
        std::string code = "void main( { int x = ; }";
        auto doc = test::CreateTestDocument("file:///test_syntax.as", code);
        SymbolTable table;
        SymbolResolver resolver;

        ValidationContext ctx{
            .document = doc,
            .symbolTable = table,
            .symbolResolver = resolver,
            .rootNode = doc.RootNode()
        };

        SyntaxErrorRule rule;
        auto diags = rule.run(ctx);

        CHECK_FALSE(diags.empty());
        CHECK(rule.getName() == "SyntaxErrorRule");
    }

    TEST_CASE("TypeValidationRule - flags unknown types in declarations")
    {
        std::string code = "voids invalidFunction() {}\nInvalidType badVar;";
        auto doc = test::CreateTestDocument("file:///test_type.as", code);
        SymbolTable table;
        SymbolResolver resolver;

        ValidationContext ctx{
            .document = doc,
            .symbolTable = table,
            .symbolResolver = resolver,
            .rootNode = doc.RootNode()
        };

        TypeValidationRule rule;
        auto diags = rule.run(ctx);

        CHECK_FALSE(diags.empty());
        bool foundVoidErr = false;
        bool foundBadTypeErr = false;
        for (const auto &d : diags)
        {
            if (d.message.find("voids") != std::string::npos) foundVoidErr = true;
            if (d.message.find("InvalidType") != std::string::npos) foundBadTypeErr = true;
        }
        CHECK(foundVoidErr);
        CHECK(foundBadTypeErr);
    }

    TEST_CASE("FunctionDeclRule - flags return value in void function and signature redefinition")
    {
        std::string code = "void testVoid() { return 42; }\nvoid testVoid() { return 100; }";
        auto doc = test::CreateTestDocument("file:///test_func.as", code);
        SymbolTable table;
        SymbolResolver resolver;

        ValidationContext ctx{
            .document = doc,
            .symbolTable = table,
            .symbolResolver = resolver,
            .rootNode = doc.RootNode()
        };

        FunctionDeclRule rule;
        auto diags = rule.run(ctx);

        CHECK_FALSE(diags.empty());
        bool foundVoidReturnErr = false;
        bool foundRedefinitionErr = false;

        for (const auto &d : diags)
        {
            if (d.message.find("function returning void") != std::string::npos) foundVoidReturnErr = true;
            if (d.message.find("Redefinition of function") != std::string::npos) foundRedefinitionErr = true;
        }
        CHECK(foundVoidReturnErr);
        CHECK(foundRedefinitionErr);
    }

    TEST_CASE("ClassDeclRule - flags missing base class and duplicate class members")
    {
        std::string code = "class Player : NonExistentBase {\n    int health;\n    float health;\n}";
        auto doc = test::CreateTestDocument("file:///test_class.as", code);
        SymbolTable table;
        SymbolResolver resolver;

        ValidationContext ctx{
            .document = doc,
            .symbolTable = table,
            .symbolResolver = resolver,
            .rootNode = doc.RootNode()
        };

        ClassDeclRule rule;
        auto diags = rule.run(ctx);

        CHECK_FALSE(diags.empty());
        bool foundBaseErr = false;
        bool foundDupMemberErr = false;

        for (const auto &d : diags)
        {
            if (d.message.find("NonExistentBase") != std::string::npos) foundBaseErr = true;
            if (d.message.find("Duplicate member 'health'") != std::string::npos) foundDupMemberErr = true;
        }
        CHECK(foundBaseErr);
        CHECK(foundDupMemberErr);
    }

    TEST_CASE("ArgumentRule - flags unknown parameter types and duplicate parameter names")
    {
        std::string code = "void process(UnknownType x, int amount, float amount) {}";
        auto doc = test::CreateTestDocument("file:///test_arg.as", code);
        SymbolTable table;
        SymbolResolver resolver;

        ValidationContext ctx{
            .document = doc,
            .symbolTable = table,
            .symbolResolver = resolver,
            .rootNode = doc.GetRootNode()
        };

        ArgumentRule rule;
        auto diags = rule.run(ctx);

        CHECK_FALSE(diags.empty());
        bool foundTypeErr = false;
        bool foundDupParamErr = false;

        for (const auto &d : diags)
        {
            if (d.message.find("UnknownType") != std::string::npos) foundTypeErr = true;
            if (d.message.find("Duplicate parameter name 'amount'") != std::string::npos) foundDupParamErr = true;
        }
        CHECK(foundTypeErr);
        CHECK(foundDupParamErr);
    }

    TEST_CASE("ValidationOracle Orchestrator - runs pipeline end-to-end")
    {
        std::string code = "class Hero : UnknownBase {\n    int hp;\n    int hp;\n}\nvoid run(UnknownType t, int a, float a) { return 5; }";
        auto doc = test::CreateTestDocument("file:///test_pipeline.as", code);
        SymbolTable table;
        test::PopulateTestSymbolTable(doc, table);

        ValidationOracle oracle;
        auto diags = oracle.ValidateSync(doc, table, table);

        CHECK_FALSE(diags.empty());
    }
}
