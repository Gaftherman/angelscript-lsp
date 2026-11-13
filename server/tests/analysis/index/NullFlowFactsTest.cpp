#include "analysis/index/NullFlowFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("NullFlowFacts - Callable bodies and declarations")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string declFnName = angel_lsp::test::GenerateRandomSymbolName("declFn");
    const std::string bodyFnName = angel_lsp::test::GenerateRandomSymbolName("bodyFn");
    const std::string lambdaVar = angel_lsp::test::GenerateRandomSymbolName("lambdaVar");

    const std::string code = std::string("void ") + declFnName + "();\n" + "void " + bodyFnName + "()\n{\n" +
                             "    auto " + lambdaVar + " = function() {};\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const angel_lsp::analysis::NullFlowFacts facts = angel_lsp::analysis::BuildNullFlowFacts(index, code);

    REQUIRE(facts.callables.size() == 2);
    const angel_lsp::analysis::NullFlowNodeId bodyId = facts.callables.front().body;
    CHECK(bodyId != angel_lsp::analysis::NoNullFlowNode);
    const angel_lsp::analysis::NullFlowNode* bodyNode = facts.Find(bodyId);
    REQUIRE(bodyNode != nullptr);
    CHECK(bodyNode->kind == angel_lsp::analysis::NullFlowNodeKind::Block);

    ts_tree_delete(tree);
}

TEST_CASE("NullFlowFacts - If statement with handle null check condition")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("Obj");
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string handleParam = angel_lsp::test::GenerateRandomSymbolName("p");
    const std::string intParam = angel_lsp::test::GenerateRandomSymbolName("n");
    const std::string method = angel_lsp::test::GenerateRandomSymbolName("m");

    const std::string code = std::string("void ") + fnName + "(" + className + "@ " + handleParam + ", int " +
                             intParam + ")\n{\n" + "    if (" + handleParam + " !is null)\n" + "    {\n" + "        " +
                             handleParam + "." + method + "();\n" + "    }\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const angel_lsp::analysis::NullFlowFacts facts = angel_lsp::analysis::BuildNullFlowFacts(index, code);

    REQUIRE(facts.callables.size() == 1);
    REQUIRE(facts.parameters.size() == 1);
    CHECK(facts.parameters.front().name == handleParam);

    const auto* bodyNode = facts.Find(facts.callables.front().body);
    REQUIRE(bodyNode != nullptr);
    CHECK(bodyNode->kind == angel_lsp::analysis::NullFlowNodeKind::Block);

    const auto* ifStmt = facts.Find(bodyNode->first);
    REQUIRE(ifStmt != nullptr);
    CHECK(ifStmt->kind == angel_lsp::analysis::NullFlowNodeKind::IfStatement);

    const auto* condNode = facts.Find(ifStmt->first);
    REQUIRE(condNode != nullptr);
    CHECK(condNode->kind == angel_lsp::analysis::NullFlowNodeKind::Binary);
    CHECK(condNode->op == angel_lsp::analysis::NullFlowOperator::NotIs);
    CHECK(condNode->complete);

    const auto* leftNode = facts.Find(condNode->first);
    REQUIRE(leftNode != nullptr);
    CHECK((leftNode->kind == angel_lsp::analysis::NullFlowNodeKind::Identifier ||
           leftNode->kind == angel_lsp::analysis::NullFlowNodeKind::ScopedIdentifier));
    CHECK(leftNode->text == handleParam);

    const auto* rightNode = facts.Find(condNode->second);
    REQUIRE(rightNode != nullptr);
    CHECK(rightNode->isNullInitializer);

    const auto* consequenceNode = facts.Find(ifStmt->second);
    REQUIRE(consequenceNode != nullptr);
    CHECK(consequenceNode->kind == angel_lsp::analysis::NullFlowNodeKind::Block);

    ts_tree_delete(tree);
}

TEST_CASE("NullFlowFacts - Variable declarations with handle and primitive types")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("Obj");
    const std::string hVar = angel_lsp::test::GenerateRandomSymbolName("h");
    const std::string nVar = angel_lsp::test::GenerateRandomSymbolName("n");
    const std::string xVar = angel_lsp::test::GenerateRandomSymbolName("x");

    const std::string code = std::string("void ") + fnName + "()\n{\n" + "    " + className + "@ " + hVar + " = " +
                             className + "();\n" + "    " + className + "@ " + nVar + " = null;\n" + "    int " + xVar +
                             " = 1;\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const angel_lsp::analysis::NullFlowFacts facts = angel_lsp::analysis::BuildNullFlowFacts(index, code);

    REQUIRE(facts.callables.size() == 1);
    const auto* bodyNode = facts.Find(facts.callables.front().body);
    REQUIRE(bodyNode != nullptr);
    CHECK(bodyNode->kind == angel_lsp::analysis::NullFlowNodeKind::Block);

    const auto* hDecl = facts.Find(bodyNode->first);
    REQUIRE(hDecl != nullptr);
    CHECK(hDecl->kind == angel_lsp::analysis::NullFlowNodeKind::VariableDeclaration);
    CHECK(hDecl->typeIsHandle == true);

    const auto* nDecl = facts.Find(hDecl->next);
    REQUIRE(nDecl != nullptr);
    CHECK(nDecl->kind == angel_lsp::analysis::NullFlowNodeKind::VariableDeclaration);
    CHECK(nDecl->typeIsHandle == true);

    const auto* xDecl = facts.Find(nDecl->next);
    REQUIRE(xDecl != nullptr);
    CHECK(xDecl->kind == angel_lsp::analysis::NullFlowNodeKind::VariableDeclaration);
    CHECK(xDecl->typeIsHandle == false);

    REQUIRE(hDecl->declaratorCount == 1);
    REQUIRE(hDecl->declaratorBegin < facts.declarators.size());
    const auto& hDeclarator = facts.declarators[hDecl->declaratorBegin];
    CHECK(hDeclarator.name == hVar);
    CHECK(hDeclarator.value != angel_lsp::analysis::NoNullFlowNode);

    REQUIRE(nDecl->declaratorCount == 1);
    REQUIRE(nDecl->declaratorBegin < facts.declarators.size());
    const auto& nDeclarator = facts.declarators[nDecl->declaratorBegin];
    CHECK(nDeclarator.name == nVar);
    const auto* nVal = facts.Find(nDeclarator.value);
    REQUIRE(nVal != nullptr);
    CHECK(nVal->isNullInitializer);

    REQUIRE(xDecl->declaratorCount == 1);
    REQUIRE(xDecl->declaratorBegin < facts.declarators.size());
    const auto& xDeclarator = facts.declarators[xDecl->declaratorBegin];
    CHECK(xDeclarator.name == xVar);

    REQUIRE(hDecl->typeNameIndex < facts.typeBaseNames.size());
    CHECK(facts.typeBaseNames[hDecl->typeNameIndex] == className);
    REQUIRE(nDecl->typeNameIndex < facts.typeBaseNames.size());
    CHECK(facts.typeBaseNames[nDecl->typeNameIndex] == className);

    ts_tree_delete(tree);
}

TEST_CASE("NullFlowFacts - Member call expressions and argument chaining")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string objVar = angel_lsp::test::GenerateRandomSymbolName("p");
    const std::string method = angel_lsp::test::GenerateRandomSymbolName("go");
    const std::string arg1 = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string arg2 = angel_lsp::test::GenerateRandomSymbolName("b");

    const std::string code = std::string("void ") + fnName + "()\n{\n" + "    " + objVar + "." + method + "(" + arg1 +
                             ", " + arg2 + ");\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const angel_lsp::analysis::NullFlowFacts facts = angel_lsp::analysis::BuildNullFlowFacts(index, code);

    REQUIRE(facts.callables.size() == 1);
    const auto* bodyNode = facts.Find(facts.callables.front().body);
    REQUIRE(bodyNode != nullptr);
    CHECK(bodyNode->kind == angel_lsp::analysis::NullFlowNodeKind::Block);

    const auto* exprStmt = facts.Find(bodyNode->first);
    REQUIRE(exprStmt != nullptr);
    CHECK(exprStmt->kind == angel_lsp::analysis::NullFlowNodeKind::ExpressionStatement);

    const auto* callNode = facts.Find(exprStmt->first);
    REQUIRE(callNode != nullptr);
    CHECK(callNode->kind == angel_lsp::analysis::NullFlowNodeKind::Call);

    const auto* memberNode = facts.Find(callNode->first);
    REQUIRE(memberNode != nullptr);
    CHECK(memberNode->kind == angel_lsp::analysis::NullFlowNodeKind::Member);

    const auto* objNode = facts.Find(memberNode->first);
    REQUIRE(objNode != nullptr);
    CHECK((objNode->kind == angel_lsp::analysis::NullFlowNodeKind::Identifier ||
           objNode->kind == angel_lsp::analysis::NullFlowNodeKind::ScopedIdentifier));
    CHECK(objNode->text == objVar);

    const auto* firstArg = facts.Find(callNode->second);
    REQUIRE(firstArg != nullptr);
    const auto* secondArg = facts.Find(firstArg->next);
    REQUIRE(secondArg != nullptr);
    CHECK(secondArg->next == angel_lsp::analysis::NoNullFlowNode);

    ts_tree_delete(tree);
}
