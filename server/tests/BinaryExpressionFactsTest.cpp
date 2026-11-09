#include "analysis/index/BinaryExpressionFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("BinaryExpressionFacts - Operator classes and spellings")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");

    const std::string code = "void " + fnName + "()\n{\n" + "    " + varA + " + " + varB + ";\n" + "    " + varA +
                             " == " + varB + ";\n" + "    " + varA + " < " + varB + ";\n" + "    " + varA + " & " +
                             varB + ";\n" + "    " + varA + " is null;\n" + "    " + varA + " !is null;\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildBinaryExpressionFacts(index, code);

    REQUIRE(facts.expressions.size() == 6);
    const angel_lsp::analysis::BinaryExpressionFact* arithFact = nullptr;
    const angel_lsp::analysis::BinaryExpressionFact* eqFact = nullptr;
    const angel_lsp::analysis::BinaryExpressionFact* relFact = nullptr;
    const angel_lsp::analysis::BinaryExpressionFact* bitFact = nullptr;
    const angel_lsp::analysis::BinaryExpressionFact* isFact = nullptr;
    const angel_lsp::analysis::BinaryExpressionFact* notIsFact = nullptr;

    for (const auto& expr : facts.expressions)
    {
        if (expr.op == "+")
            arithFact = &expr;
        else if (expr.op == "==")
            eqFact = &expr;
        else if (expr.op == "<")
            relFact = &expr;
        else if (expr.op == "&")
            bitFact = &expr;
        else if (expr.op == "is")
            isFact = &expr;
        else if (expr.op == "!is")
            notIsFact = &expr;
    }

    REQUIRE(arithFact != nullptr);
    CHECK(arithFact->op == "+");
    CHECK(arithFact->opClass == angel_lsp::analysis::BinaryOperatorClass::Arithmetic);

    REQUIRE(eqFact != nullptr);
    CHECK(eqFact->op == "==");
    CHECK(eqFact->opClass == angel_lsp::analysis::BinaryOperatorClass::Equality);

    REQUIRE(relFact != nullptr);
    CHECK(relFact->op == "<");
    CHECK(relFact->opClass == angel_lsp::analysis::BinaryOperatorClass::Relational);

    REQUIRE(bitFact != nullptr);
    CHECK(bitFact->op == "&");
    CHECK(bitFact->opClass == angel_lsp::analysis::BinaryOperatorClass::Bitwise);

    REQUIRE(isFact != nullptr);
    CHECK(isFact->op == "is");
    CHECK(isFact->opClass == angel_lsp::analysis::BinaryOperatorClass::Identity);

    REQUIRE(notIsFact != nullptr);
    CHECK(notIsFact->op == "!is");
    CHECK(notIsFact->opClass == angel_lsp::analysis::BinaryOperatorClass::Identity);

    ts_tree_delete(tree);
}

TEST_CASE("BinaryExpressionFacts - Operand flags")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Cls");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("method");
    const std::string handleVar = angel_lsp::test::GenerateRandomSymbolName("h");
    const std::string objVar = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string memberVar = angel_lsp::test::GenerateRandomSymbolName("mem");

    const std::string code = "class " + clsName + "\n{\n" + "    void " + methodName + "()\n    {\n" + "        " +
                             handleVar + " is null;\n" + "        this == " + handleVar + ";\n" + "        @" +
                             handleVar + " == " + handleVar + ";\n" + "        " + objVar + "." + memberVar +
                             " == 1;\n" + "    }\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildBinaryExpressionFacts(index, code);

    REQUIRE(facts.expressions.size() == 4);

    const auto& nullExpr = facts.expressions[0];
    CHECK_FALSE(nullExpr.left.isNullLiteral);
    CHECK(nullExpr.right.isNullLiteral);

    const auto& thisExpr = facts.expressions[1];
    CHECK(thisExpr.left.isThis);
    CHECK_FALSE(thisExpr.right.isThis);

    const auto& handleOpExpr = facts.expressions[2];
    CHECK(handleOpExpr.left.hasHandleOperator);
    CHECK_FALSE(handleOpExpr.right.hasHandleOperator);

    const auto& memberLitExpr = facts.expressions[3];
    CHECK(memberLitExpr.left.isExtractableCandidate);
    CHECK(memberLitExpr.right.isNumericLiteral);
    CHECK_FALSE(memberLitExpr.left.isNumericLiteral);

    ts_tree_delete(tree);
}

TEST_CASE("BinaryExpressionFacts - Ranges and text")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");
    const std::string exprText = varA + " + " + varB;

    const std::string code = "void " + fnName + "()\n{\n    " + exprText + ";\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildBinaryExpressionFacts(index, code);

    REQUIRE(facts.expressions.size() == 1);
    const auto& exprFact = facts.expressions.front();

    CHECK(exprFact.left.text == varA);
    CHECK(exprFact.right.text == varB);

    const size_t expectedStart = code.find(exprText);
    REQUIRE(expectedStart != std::string::npos);
    CHECK(exprFact.bytes.startByte == expectedStart);
    CHECK(exprFact.bytes.endByte == expectedStart + exprText.size());

    CHECK(exprFact.operatorRange.startLine == exprFact.left.range.startLine);
    CHECK(exprFact.operatorRange.startLine == exprFact.right.range.startLine);

    ts_tree_delete(tree);
}

TEST_CASE("BinaryExpressionFacts - Find and nesting")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");
    const std::string varC = angel_lsp::test::GenerateRandomSymbolName("c");

    const std::string code = "void " + fnName + "()\n{\n    " + varA + " + " + varB + " + " + varC + ";\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildBinaryExpressionFacts(index, code);

    REQUIRE(facts.expressions.size() == 2);
    const auto& outer = facts.expressions[0];
    const auto& inner = facts.expressions[1];

    const auto* foundInner = facts.Find(inner.bytes);
    REQUIRE(foundInner != nullptr);
    CHECK(foundInner == &inner);

    const auto* foundOuter = facts.Find(outer.bytes);
    REQUIRE(foundOuter != nullptr);
    CHECK(foundOuter == &outer);

    const angel_lsp::analysis::IndexedByteRange nonExprSpan{inner.bytes.startByte,
                                                            inner.bytes.startByte + static_cast<uint32_t>(varA.size())};
    CHECK(facts.Find(nonExprSpan) == nullptr);
    CHECK(facts.Find(angel_lsp::analysis::IndexedByteRange{0, 0}) == nullptr);

    const auto within = facts.FindWithin(outer.bytes);
    REQUIRE(within.size() == 2);
    REQUIRE(within[0] != nullptr);
    REQUIRE(within[1] != nullptr);
    CHECK(within[0] == &outer);
    CHECK(within[1] == &inner);

    ts_tree_delete(tree);
}

TEST_CASE("BinaryExpressionFacts - Ladders and nested else ladders")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string cond1 = angel_lsp::test::GenerateRandomSymbolName("condA");
    const std::string cond2 = angel_lsp::test::GenerateRandomSymbolName("condB");
    const std::string cond3 = angel_lsp::test::GenerateRandomSymbolName("condC");
    const std::string cond4 = angel_lsp::test::GenerateRandomSymbolName("condD");

    const std::string code = "void " + fnName + "()\n{\n" + "    if (" + cond1 + ")\n    {\n    }\n" + "    else if (" +
                             cond2 + ")\n    {\n    }\n" + "    else if (" + cond3 + ")\n    {\n    }\n" +
                             "    else\n    {\n" + "        if (" + cond4 + ")\n        {\n        }\n" + "    }\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildBinaryExpressionFacts(index, code);

    REQUIRE(facts.ladders.size() == 2);

    const auto& outerLadder = facts.ladders[0];
    CHECK(outerLadder.conditionCount == 3);
    REQUIRE(outerLadder.conditionBegin + outerLadder.conditionCount <= facts.ladderConditions.size());

    const auto condSpan1 = facts.ladderConditions[outerLadder.conditionBegin];
    const auto condSpan2 = facts.ladderConditions[outerLadder.conditionBegin + 1];
    const auto condSpan3 = facts.ladderConditions[outerLadder.conditionBegin + 2];

    CHECK(condSpan1.startByte < condSpan2.startByte);
    CHECK(condSpan2.startByte < condSpan3.startByte);
    CHECK(code.substr(condSpan1.startByte, condSpan1.endByte - condSpan1.startByte) == cond1);
    CHECK(code.substr(condSpan2.startByte, condSpan2.endByte - condSpan2.startByte) == cond2);
    CHECK(code.substr(condSpan3.startByte, condSpan3.endByte - condSpan3.startByte) == cond3);

    const auto* foundRoot = facts.FindLadder(outerLadder.rootIf);
    REQUIRE(foundRoot != nullptr);
    CHECK(foundRoot == &outerLadder);

    const auto& nestedLadder = facts.ladders[1];
    CHECK(nestedLadder.conditionCount == 1);
    REQUIRE(nestedLadder.conditionBegin + nestedLadder.conditionCount <= facts.ladderConditions.size());

    const auto condSpan4 = facts.ladderConditions[nestedLadder.conditionBegin];
    CHECK(code.substr(condSpan4.startByte, condSpan4.endByte - condSpan4.startByte) == cond4);

    const auto* foundNested = facts.FindLadder(nestedLadder.rootIf);
    REQUIRE(foundNested != nullptr);
    CHECK(foundNested == &nestedLadder);

    const size_t elseIf2Pos = code.find("if (" + cond2 + ")");
    REQUIRE(elseIf2Pos != std::string::npos);
    const angel_lsp::analysis::IndexedByteRange elseIf2Span{static_cast<uint32_t>(elseIf2Pos),
                                                            static_cast<uint32_t>(code.find("else", elseIf2Pos))};
    CHECK(facts.FindLadder(elseIf2Span) == nullptr);

    ts_tree_delete(tree);
}
