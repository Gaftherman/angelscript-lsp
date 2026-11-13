#include "analysis/index/ConstSiteFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("ConstSiteFacts - Assignment targets classification and handle flag")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varX = angel_lsp::test::GenerateRandomSymbolName("x");
    const std::string varH = angel_lsp::test::GenerateRandomSymbolName("h");
    const std::string varObj = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string varField = angel_lsp::test::GenerateRandomSymbolName("field");
    const std::string varArr = angel_lsp::test::GenerateRandomSymbolName("arr");

    const std::string code = "void " + fnName + "()\n{\n" + "    " + varX + " = 10;\n" + "    @" + varH + " = " +
                             varObj + ";\n" + "    this = " + varObj + ";\n" + "    this." + varField + " = 20;\n" +
                             "    " + varObj + "." + varField + " = 30;\n" + "    " + varArr + "[0] = 40;\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildConstSiteFacts(index, code);

    REQUIRE(facts.assignments.size() == 6);

    // 1. Plain identifier assignment
    const auto& assign0 = facts.assignments[0];
    CHECK(assign0.target.kind == angel_lsp::analysis::ConstTargetKind::Identifier);
    CHECK_FALSE(assign0.target.isHandle);
    CHECK(assign0.target.text == varX);
    CHECK(assign0.target.actualTargetText == varX);
    CHECK(assign0.target.leafText == varX);
    CHECK(assign0.target.leafIsThis == false);
    CHECK(assign0.target.inMemberChain == false);
    CHECK(facts.FindAssignment(assign0.bytes) == &assign0);

    // 2. Handle assignment
    const auto& assign1 = facts.assignments[1];
    CHECK(assign1.target.kind == angel_lsp::analysis::ConstTargetKind::Identifier);
    CHECK(assign1.target.isHandle);
    CHECK(assign1.target.actualTargetText == varH);
    CHECK(assign1.target.leafText == varH);
    CHECK(assign1.target.leafIsThis == false);
    CHECK(assign1.target.inMemberChain == false);

    // 3. Direct this assignment
    const auto& assign2 = facts.assignments[2];
    CHECK(assign2.target.kind == angel_lsp::analysis::ConstTargetKind::This);
    CHECK(assign2.target.leafIsThis == true);

    // 4. this-member assignment
    const auto& assign3 = facts.assignments[3];
    CHECK(assign3.target.kind == angel_lsp::analysis::ConstTargetKind::ThisMember);
    CHECK(assign3.target.memberName == varField);
    CHECK(assign3.target.receiverText == "this");
    CHECK(assign3.target.leafIsThis == true);
    CHECK(assign3.target.inMemberChain == true);

    // 5. Member assignment
    const auto& assign4 = facts.assignments[4];
    CHECK(assign4.target.kind == angel_lsp::analysis::ConstTargetKind::Member);
    CHECK(assign4.target.memberName == varField);
    CHECK(assign4.target.receiverText == varObj);
    CHECK(assign4.target.leafText == varObj);
    CHECK(assign4.target.leafIsThis == false);
    CHECK(assign4.target.inMemberChain == true);

    // 6. Index assignment
    const auto& assign5 = facts.assignments[5];
    CHECK(assign5.target.kind == angel_lsp::analysis::ConstTargetKind::Index);

    ts_tree_delete(tree);
}

TEST_CASE("ConstSiteFacts - Method call sites")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varObj = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string methodA = angel_lsp::test::GenerateRandomSymbolName("methodA");
    const std::string methodB = angel_lsp::test::GenerateRandomSymbolName("methodB");

    const std::string code = "void " + fnName + "()\n{\n" + "    " + varObj + "." + methodA + "();\n" + "    this." +
                             methodB + "();\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildConstSiteFacts(index, code);

    REQUIRE(facts.methodCalls.size() == 2);

    const auto& call0 = facts.methodCalls[0];
    CHECK(call0.methodName == methodA);
    CHECK(call0.object.text == varObj);
    CHECK(call0.leafText == varObj);
    CHECK(call0.leafIsThis == false);
    CHECK(facts.FindMethodCall(call0.bytes) == &call0);

    const auto& call1 = facts.methodCalls[1];
    CHECK(call1.methodName == methodB);
    CHECK(call1.object.text == "this");
    CHECK(call1.leafIsThis == true);
    CHECK(facts.FindMethodCall(call1.bytes) == &call1);

    ts_tree_delete(tree);
}

TEST_CASE("ConstSiteFacts - Enclosing methods and parameter declarations")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Cls");
    const std::string constMethod = angel_lsp::test::GenerateRandomSymbolName("constMethod");
    const std::string mutMethod = angel_lsp::test::GenerateRandomSymbolName("mutMethod");
    const std::string freeFn = angel_lsp::test::GenerateRandomSymbolName("freeFn");
    const std::string paramA = angel_lsp::test::GenerateRandomSymbolName("paramA");
    const std::string paramB = angel_lsp::test::GenerateRandomSymbolName("paramB");
    const std::string paramC = angel_lsp::test::GenerateRandomSymbolName("paramC");
    const std::string paramD = angel_lsp::test::GenerateRandomSymbolName("paramD");
    const std::string paramE = angel_lsp::test::GenerateRandomSymbolName("paramE");

    const std::string code = "class " + clsName + "\n{\n" + "    void " + constMethod + "(const int " + paramA +
                             ", Handle@ const " + paramB + ", float " + paramC + ") const\n    {\n" + "        " +
                             paramA + ";\n" + "    }\n" + "    void " + mutMethod + "(int " + paramD + ")\n    {\n" +
                             "        " + paramD + ";\n" + "    }\n" + "}\n" + "void " + freeFn + "(const float " +
                             paramE + ")\n{\n" + "    " + paramE + ";\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildConstSiteFacts(index, code);

    REQUIRE(facts.enclosingMethods.size() == 3);

    const angel_lsp::analysis::EnclosingMethodFact* constFact = nullptr;
    const angel_lsp::analysis::EnclosingMethodFact* mutFact = nullptr;
    const angel_lsp::analysis::EnclosingMethodFact* freeFact = nullptr;

    for (const auto& m : facts.enclosingMethods)
    {
        if (m.methodName == constMethod)
        {
            constFact = &m;
        }
        else if (m.methodName == mutMethod)
        {
            mutFact = &m;
        }
        else if (m.methodName == freeFn)
        {
            freeFact = &m;
        }
    }

    REQUIRE(constFact != nullptr);
    CHECK(constFact->className == clsName);
    CHECK(constFact->inClass == true);
    CHECK(constFact->isConstMethod == true);
    CHECK(constFact->parameterCount == 3);

    const auto constParams = facts.ParametersOf(*constFact);
    REQUIRE(constParams.size() == 3);
    CHECK(constParams[0].name == paramA);
    CHECK(constParams[0].isConst);
    CHECK(constParams[0].isHandleConst == false);
    CHECK(constParams[1].name == paramB);
    CHECK_FALSE(constParams[1].isConst);
    CHECK(constParams[1].isHandleConst == true);
    CHECK(constParams[2].name == paramC);
    CHECK_FALSE(constParams[2].isConst);
    CHECK(constParams[2].isHandleConst == false);

    REQUIRE(mutFact != nullptr);
    CHECK(mutFact->className == clsName);
    CHECK(mutFact->inClass == true);
    CHECK(mutFact->isConstMethod == false);
    CHECK(mutFact->parameterCount == 1);

    REQUIRE(freeFact != nullptr);
    CHECK(freeFact->className.empty());
    CHECK(freeFact->inClass == false);
    CHECK(freeFact->isConstMethod == false);
    CHECK(freeFact->parameterCount == 1);

    // Test FindEnclosingMethodAt
    const uint32_t constBodyOffset = static_cast<uint32_t>(code.find(paramA + ";"));
    const auto* encAtConst = facts.FindEnclosingMethodAt(constBodyOffset);
    REQUIRE(encAtConst != nullptr);
    CHECK(encAtConst->methodName == constMethod);
    CHECK(encAtConst->isConstMethod == true);

    const uint32_t freeBodyOffset = static_cast<uint32_t>(code.find(paramE + ";"));
    const auto* encAtFree = facts.FindEnclosingMethodAt(freeBodyOffset);
    REQUIRE(encAtFree != nullptr);
    CHECK(encAtFree->methodName == freeFn);
    CHECK(encAtFree->inClass == false);

    // Test FindParameter
    const auto* paramAFact = facts.FindParameter(constBodyOffset, paramA);
    REQUIRE(paramAFact != nullptr);
    CHECK(paramAFact->isConst == true);
    CHECK(paramAFact->isHandleConst == false);

    const auto* paramBFact = facts.FindParameter(constBodyOffset, paramB);
    REQUIRE(paramBFact != nullptr);
    CHECK(paramBFact->isConst == false);
    CHECK(paramBFact->isHandleConst == true);

    const auto* paramEFact = facts.FindParameter(freeBodyOffset, paramE);
    REQUIRE(paramEFact != nullptr);
    CHECK(paramEFact->isConst == true);
    CHECK(paramEFact->isHandleConst == false);

    ts_tree_delete(tree);
}

TEST_CASE("ConstSiteFacts - Lambda parameter scope")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varFn = angel_lsp::test::GenerateRandomSymbolName("lam");
    const std::string paramX = angel_lsp::test::GenerateRandomSymbolName("px");

    const std::string code = "void " + fnName + "()\n{\n" + "    auto " + varFn + " = function(const int " + paramX +
                             ") {\n" + "        " + paramX + ";\n" + "    };\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildConstSiteFacts(index, code);

    const uint32_t lamBodyOffset = static_cast<uint32_t>(code.find(paramX + ";"));
    const auto* paramFact = facts.FindParameter(lamBodyOffset, paramX);
    REQUIRE(paramFact != nullptr);
    CHECK(paramFact->name == paramX);
    CHECK(paramFact->isConst == true);
    CHECK(paramFact->isHandleConst == false);

    ts_tree_delete(tree);
}
