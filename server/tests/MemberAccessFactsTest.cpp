#include "analysis/index/MemberAccessFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("MemberAccessFacts - Member expressions and receiver operands")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Cls");
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string objVar = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string fieldA = angel_lsp::test::GenerateRandomSymbolName("fieldA");
    const std::string fieldB = angel_lsp::test::GenerateRandomSymbolName("fieldB");

    const std::string code = "class " + clsName + "\n{\n" + "    void " + fnName + "()\n    {\n" + "        this." +
                             fieldA + ";\n" + "        " + objVar + "." + fieldB + ";\n" + "    }\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildMemberAccessFacts(index, code);

    REQUIRE(facts.memberExpressions.size() == 2);

    const auto* thisExpr = facts.FindMemberExpression(facts.memberExpressions[0].bytes);
    REQUIRE(thisExpr != nullptr);
    CHECK(thisExpr->memberName == fieldA);
    CHECK(thisExpr->objectText == "this");
    CHECK(thisExpr->isThisAccess == true);
    CHECK(thisExpr->enclosingClassName == clsName);
    CHECK(thisExpr->insideMixin == false);

    const auto* objExpr = facts.FindMemberExpression(facts.memberExpressions[1].bytes);
    REQUIRE(objExpr != nullptr);
    CHECK(objExpr->memberName == fieldB);
    CHECK(objExpr->objectText == objVar);
    CHECK(objExpr->isThisAccess == false);
    CHECK(objExpr->enclosingClassName == clsName);

    ts_tree_delete(tree);
}

TEST_CASE("MemberAccessFacts - Identifier skip rules and filtered candidates")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("varA");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("varB");
    const std::string objVar = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string memberName = angel_lsp::test::GenerateRandomSymbolName("mem");
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("NS");
    const std::string nsVar = angel_lsp::test::GenerateRandomSymbolName("nsVar");

    const std::string code = "void " + fnName + "()\n{\n" + "    int " + varA + " = 10; // comment\n" + "    " + varB +
                             " = " + varA + ";\n" + "    " + objVar + "." + memberName + ";\n" + "    " + nsName +
                             "::" + nsVar + ";\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildMemberAccessFacts(index, code);

    // Declaration name `varA` and type `int` must be skipped.
    // In `objVar.memberName`, memberName must be skipped (handled as member expression).
    // In `nsName::nsVar`, the leaf `nsVar` must be skipped, but `nsName::nsVar` is present.
    bool foundVarB = false;
    bool foundVarAUsage = false;
    bool foundObjVar = false;
    bool foundMemberName = false;
    bool foundScoped = false;

    for (const auto& idFact : facts.identifiers)
    {
        if (idFact.name == varB)
        {
            foundVarB = true;
        }
        else if (idFact.name == varA)
        {
            foundVarAUsage = true;
        }
        else if (idFact.name == objVar)
        {
            foundObjVar = true;
        }
        else if (idFact.name == memberName)
        {
            foundMemberName = true;
        }
        else if (idFact.name == nsName + "::" + nsVar)
        {
            foundScoped = true;
        }
        CHECK(facts.FindIdentifier(idFact.bytes) != nullptr);
    }

    CHECK(foundVarB);
    CHECK(foundVarAUsage);
    CHECK(foundObjVar);
    CHECK_FALSE(foundMemberName);
    CHECK(foundScoped);

    ts_tree_delete(tree);
}

TEST_CASE("MemberAccessFacts - Lambda enclosures and closure enclosure spans")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Cls");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("method");
    const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("param");

    const std::string code = "class " + clsName + "\n{\n" + "    void " + methodName + "()\n    {\n" +
                             "        auto fn = function(" + paramName + ") { return " + paramName + "; };\n" +
                             "    }\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildMemberAccessFacts(index, code);

    REQUIRE(facts.lambdaEnclosures.size() == 1);
    const auto& lambdaFact = facts.lambdaEnclosures[0];
    CHECK(lambdaFact.enclosingClassName == clsName);
    CHECK(lambdaFact.identifierCount > 0);
    CHECK(facts.FindLambdaEnclosure(lambdaFact.bytes) != nullptr);

    bool foundInnerParam = false;
    for (const auto& idFact : facts.identifiers)
    {
        if (idFact.name == paramName)
        {
            foundInnerParam = true;
            CHECK(idFact.isInsideLambda == true);
            CHECK(idFact.enclosingClassOfLambda == clsName);
            CHECK(idFact.enclosingClassName.empty()); // Blocked by closure barrier.
        }
    }
    CHECK(foundInnerParam);

    ts_tree_delete(tree);
}

TEST_CASE("MemberAccessFacts - Implicit member access inputs and mixin flag")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("RegularCls");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("MixinCls");
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("run");
    const std::string fieldA = angel_lsp::test::GenerateRandomSymbolName("fieldA");
    const std::string fieldB = angel_lsp::test::GenerateRandomSymbolName("fieldB");

    const std::string code = "class " + clsName + "\n{\n" + "    void " + fnName + "()\n    {\n" + "        " + fieldA +
                             " = 1;\n" + "    }\n}\n" + "mixin class " + mixinName + "\n{\n" + "    void " + fnName +
                             "()\n    {\n" + "        " + fieldB + " = 2;\n" + "    }\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildMemberAccessFacts(index, code);

    const angel_lsp::analysis::ImplicitMemberAccessFact* accessA = nullptr;
    const angel_lsp::analysis::ImplicitMemberAccessFact* accessB = nullptr;

    for (const auto& impl : facts.implicitAccesses)
    {
        if (impl.name == fieldA)
        {
            accessA = &impl;
        }
        else if (impl.name == fieldB)
        {
            accessB = &impl;
        }
    }

    REQUIRE(accessA != nullptr);
    CHECK(accessA->enclosingClassName == clsName);
    CHECK(accessA->insideMixin == false);
    CHECK(facts.FindImplicitAccess(accessA->bytes) != nullptr);

    REQUIRE(accessB != nullptr);
    CHECK(accessB->enclosingClassName == mixinName);
    CHECK(accessB->insideMixin == true);
    CHECK(facts.FindImplicitAccess(accessB->bytes) != nullptr);

    ts_tree_delete(tree);
}
