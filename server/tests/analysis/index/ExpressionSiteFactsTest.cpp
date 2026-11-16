#include "analysis/index/ExpressionSiteFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE("analysis/index/ExpressionSiteFacts")
{
    TEST_CASE("ExpressionSiteFacts - Binary sites and folded constants")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
        const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");
        const std::string varConst = angel_lsp::test::GenerateRandomSymbolName("kVal");

        const std::string code = "void " + fnName + "()\n{\n" + "    " + varA + " + " + varB + ";\n" + "    " + varA +
                                 " == " + varB + ";\n" + "    int x = (((42)));\n" + "    int y = ((" + varConst +
                                 "));\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildExpressionSiteFacts(index, code);

        REQUIRE(facts.binaryExpressions.size() == 2);
        const auto& bin0 = facts.binaryExpressions[0];
        const auto* foundBin0 = facts.FindBinary(bin0.bytes);
        REQUIRE(foundBin0 != nullptr);
        CHECK(facts.RangeOf(foundBin0->bytes).startLine == 2);

        bool foundLiteralChain = false;
        bool foundIdentifierChain = false;
        for (const auto& fc : facts.foldedConstants)
        {
            if (fc.isNumericLiteral && fc.chainCount >= 3)
            {
                foundLiteralChain = true;
                const auto* lookup = facts.FindFoldedConstant(fc.bytes);
                REQUIRE(lookup != nullptr);
                CHECK(lookup->isNumericLiteral);
                CHECK(facts.UnwrappedSpansOf(*lookup).size() >= 3);
            }
            if (facts.TextOf(fc.foldedNameBytes) == varConst && fc.chainCount >= 2)
            {
                foundIdentifierChain = true;
                const auto* lookup = facts.FindFoldedConstant(fc.bytes);
                REQUIRE(lookup != nullptr);
                CHECK(facts.TextOf(lookup->foldedNameBytes) == varConst);
            }
        }
        CHECK(foundLiteralChain);
        CHECK(foundIdentifierChain);
        CHECK(!facts.unwrappedSpans.empty());

        ts_tree_delete(tree);
    }

    TEST_CASE("ExpressionSiteFacts - Unary and postfix expressions")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string varX = angel_lsp::test::GenerateRandomSymbolName("x");
        const std::string varY = angel_lsp::test::GenerateRandomSymbolName("y");
        const std::string handleVar = angel_lsp::test::GenerateRandomSymbolName("h");
        const std::string arrVar = angel_lsp::test::GenerateRandomSymbolName("arr");

        const std::string code = "void " + fnName + "()\n{\n" + "    ++" + varX + ";\n" + "    " + varY + "--;\n" +
                                 "    @" + handleVar + ";\n" + "    " + arrVar + "[0]++;\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildExpressionSiteFacts(index, code);

        REQUIRE(facts.unaryPostfixExpressions.size() == 4);

        const angel_lsp::analysis::UnaryPostfixSiteFact* preInc = nullptr;
        const angel_lsp::analysis::UnaryPostfixSiteFact* postDec = nullptr;
        const angel_lsp::analysis::UnaryPostfixSiteFact* handleAt = nullptr;
        const angel_lsp::analysis::UnaryPostfixSiteFact* indexedInc = nullptr;

        for (const auto& expr : facts.unaryPostfixExpressions)
        {
            if (expr.op == angel_lsp::analysis::UnaryPostfixOp::PlusPlus && !expr.isPostfix)
            {
                preInc = &expr;
            }
            else if (expr.op == angel_lsp::analysis::UnaryPostfixOp::MinusMinus && expr.isPostfix)
            {
                postDec = &expr;
            }
            else if (expr.op == angel_lsp::analysis::UnaryPostfixOp::At)
            {
                handleAt = &expr;
            }
            else if (expr.op == angel_lsp::analysis::UnaryPostfixOp::PlusPlus && expr.isPostfix)
            {
                indexedInc = &expr;
            }
        }

        REQUIRE(preInc != nullptr);
        CHECK(!preInc->isPostfix);
        CHECK(preInc->isIncrementOrDecrement);
        CHECK(!preInc->isAddressOfHandle);
        CHECK(preInc->hasOperand);
        CHECK(facts.TextOf(preInc->operandBytes) == varX);
        CHECK(!preInc->operandIsIndexed);

        REQUIRE(postDec != nullptr);
        CHECK(postDec->isPostfix);
        CHECK(postDec->isIncrementOrDecrement);
        CHECK(postDec->hasOperand);
        CHECK(facts.TextOf(postDec->operandBytes) == varY);

        REQUIRE(handleAt != nullptr);
        CHECK(!handleAt->isPostfix);
        CHECK(!handleAt->isIncrementOrDecrement);
        CHECK(handleAt->isAddressOfHandle);
        CHECK(handleAt->hasOperand);
        CHECK(facts.TextOf(handleAt->operandBytes) == handleVar);

        REQUIRE(indexedInc != nullptr);
        CHECK(indexedInc->isPostfix);
        CHECK(indexedInc->isIncrementOrDecrement);
        CHECK(indexedInc->operandIsIndexed);
        CHECK(facts.OperandPropertyOf(*indexedInc).kind == angel_lsp::analysis::PropertyAccessKind::Name);

        ts_tree_delete(tree);
    }

    TEST_CASE("ExpressionSiteFacts - Member and index expressions")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string objVar = angel_lsp::test::GenerateRandomSymbolName("obj");
        const std::string memProp = angel_lsp::test::GenerateRandomSymbolName("prop");
        const std::string memMethod = angel_lsp::test::GenerateRandomSymbolName("method");
        const std::string arrVar = angel_lsp::test::GenerateRandomSymbolName("arr");

        const std::string code = "void " + fnName + "()\n{\n" + "    " + objVar + "." + memProp + ";\n" + "    " +
                                 objVar + "." + memMethod + "();\n" + "    " + objVar + "." + memProp + " = 1;\n" +
                                 "    " + arrVar + "[0];\n" + "    " + arrVar + "[1] = 2;\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildExpressionSiteFacts(index, code);

        REQUIRE(facts.memberExpressions.size() == 3);
        const auto* readMember = &facts.memberExpressions[0];
        const auto* callMember = &facts.memberExpressions[1];
        const auto* writeMember = &facts.memberExpressions[2];

        CHECK(!readMember->isCallCallee);
        CHECK(!readMember->isWriteOrMutation);
        CHECK(facts.PropertyAccessOf(*readMember).kind == angel_lsp::analysis::PropertyAccessKind::Member);
        CHECK(facts.PropertyAccessOf(*readMember).text == memProp);

        CHECK(callMember->isCallCallee);
        CHECK(!callMember->isWriteOrMutation);

        CHECK(!writeMember->isCallCallee);
        CHECK(writeMember->isWriteOrMutation);

        const auto* foundMember = facts.FindMember(readMember->bytes);
        REQUIRE(foundMember != nullptr);
        CHECK(facts.PropertyAccessOf(*foundMember).text == memProp);

        REQUIRE(facts.indexExpressions.size() == 2);
        const auto* readIndex = &facts.indexExpressions[0];
        const auto* writeIndex = &facts.indexExpressions[1];

        CHECK(!readIndex->isWriteOrMutation);
        CHECK(facts.ObjectPropertyOf(*readIndex).kind == angel_lsp::analysis::PropertyAccessKind::Name);
        CHECK(facts.ObjectPropertyOf(*readIndex).text == arrVar);
        CHECK(facts.IndexArgsOf(*readIndex).size() == 1);
        CHECK(facts.TextOf(facts.IndexArgsOf(*readIndex)[0]) == "0");

        CHECK(writeIndex->isWriteOrMutation);

        const auto* foundIndex = facts.FindIndex(readIndex->bytes);
        REQUIRE(foundIndex != nullptr);
        CHECK(facts.ObjectPropertyOf(*foundIndex).text == arrVar);

        ts_tree_delete(tree);
    }

    TEST_CASE("ExpressionSiteFacts - Cast expressions")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string srcVar = angel_lsp::test::GenerateRandomSymbolName("src");
        const std::string targetType = angel_lsp::test::GenerateRandomSymbolName("TargetType");

        const std::string code = "void " + fnName + "()\n{\n" + "    cast<" + targetType + ">(" + srcVar + ");\n" +
                                 "    cast<int>(3.14f);\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildExpressionSiteFacts(index, code);

        REQUIRE(facts.castExpressions.size() == 2);
        const auto& cast0 = facts.castExpressions[0];
        CHECK(facts.TextOf(cast0.targetTypeBytes) == targetType);
        CHECK(cast0.hasValue);
        CHECK(facts.ValueOf(cast0).text == srcVar);

        const auto* foundCast = facts.FindCast(cast0.bytes);
        REQUIRE(foundCast != nullptr);
        CHECK(facts.TextOf(foundCast->targetTypeBytes) == targetType);

        const auto& cast1 = facts.castExpressions[1];
        CHECK(facts.TextOf(cast1.targetTypeBytes) == "int");
        CHECK(cast1.hasValue);
        CHECK(facts.ValueOf(cast1).kind == angel_lsp::analysis::ValueExpressionKind::Number);

        ts_tree_delete(tree);
    }
}
