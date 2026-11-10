#include "analysis/index/ControlFlowFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("ControlFlowFacts - Function declarations, lambdas and implicit void")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnNameA = angel_lsp::test::GenerateRandomSymbolName("fnA");
    const std::string fnNameB = angel_lsp::test::GenerateRandomSymbolName("fnB");
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Cls");

    const std::string code = "int " + fnNameA + "()\n{\n    return 42;\n}\n\n" + "void " + fnNameB +
                             "()\n{\n    return;\n}\n\n" + "class " + clsName + "\n{\n    " + clsName +
                             "()\n    {\n    }\n}\n" +
                             "void testLambda()\n{\n    auto l = function() { return 1; };\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildControlFlowFacts(index, code);

    REQUIRE(facts.functions.size() >= 4);

    const angel_lsp::analysis::ControlFlowFunctionFact* factA = nullptr;
    const angel_lsp::analysis::ControlFlowFunctionFact* factB = nullptr;
    const angel_lsp::analysis::ControlFlowFunctionFact* factCtor = nullptr;
    const angel_lsp::analysis::ControlFlowFunctionFact* factLambda = nullptr;

    for (const auto& fn : facts.functions)
    {
        if (fn.name == fnNameA)
        {
            factA = &fn;
        }
        else if (fn.name == fnNameB)
        {
            factB = &fn;
        }
        else if (fn.name == clsName)
        {
            factCtor = &fn;
        }
        else if (fn.isLambda)
        {
            factLambda = &fn;
        }
    }

    REQUIRE(factA != nullptr);
    CHECK(factA->returnType == "int");
    CHECK_FALSE(factA->implicitVoid);
    CHECK(factA->definitelyReturns);
    CHECK_FALSE(factA->isLambda);
    CHECK(facts.FindFunction(factA->bytes) == factA);

    REQUIRE(factB != nullptr);
    CHECK(factB->returnType == "void");
    CHECK_FALSE(factB->implicitVoid);
    CHECK(factB->definitelyReturns);
    CHECK(facts.FindFunction(factB->bytes) == factB);

    REQUIRE(factCtor != nullptr);
    CHECK(factCtor->returnType.empty());
    CHECK(factCtor->implicitVoid);
    CHECK_FALSE(factCtor->isLambda);
    CHECK(facts.FindFunction(factCtor->bytes) == factCtor);

    REQUIRE(factLambda != nullptr);
    CHECK(factLambda->isLambda);
    CHECK_FALSE(factLambda->implicitVoid);
    CHECK(factLambda->definitelyReturns);
    CHECK(facts.FindFunction(factLambda->bytes) == factLambda);

    ts_tree_delete(tree);
}

TEST_CASE("ControlFlowFacts - Statement hierarchy, parent links and child slices")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string condVar = angel_lsp::test::GenerateRandomSymbolName("cond");

    const std::string code = "void " + fnName + "()\n{\n" + "    while (" + condVar + ")\n" + "    {\n" +
                             "        if (" + condVar + ")\n" + "        {\n" + "            break;\n" + "        }\n" +
                             "    }\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildControlFlowFacts(index, code);

    REQUIRE_FALSE(facts.records.empty());

    const angel_lsp::analysis::ControlFlowRecord* whileRec = nullptr;
    const angel_lsp::analysis::ControlFlowRecord* ifRec = nullptr;
    const angel_lsp::analysis::ControlFlowRecord* breakRec = nullptr;

    for (const auto& rec : facts.records)
    {
        if (rec.kind == angel_lsp::analysis::ControlFlowRecordKind::While)
        {
            whileRec = &rec;
        }
        else if (rec.kind == angel_lsp::analysis::ControlFlowRecordKind::If)
        {
            ifRec = &rec;
        }
        else if (rec.kind == angel_lsp::analysis::ControlFlowRecordKind::Break)
        {
            breakRec = &rec;
        }
    }

    REQUIRE(whileRec != nullptr);
    REQUIRE(ifRec != nullptr);
    REQUIRE(breakRec != nullptr);

    CHECK(whileRec->loopDepth == 0);
    CHECK(breakRec->loopDepth >= 1);
    CHECK(breakRec->parentIndex != UINT32_MAX);

    const auto whileChildren = facts.ChildrenOf(*whileRec);
    CHECK_FALSE(whileChildren.empty());

    const auto* foundRec = facts.FindRecord(whileRec->bytes);
    REQUIRE(foundRec != nullptr);
    CHECK(foundRec == whileRec);

    ts_tree_delete(tree);
}

TEST_CASE("ControlFlowFacts - Return statements and enclosing function types")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnNameA = angel_lsp::test::GenerateRandomSymbolName("fnA");
    const std::string fnNameB = angel_lsp::test::GenerateRandomSymbolName("fnB");
    const std::string fnNameC = angel_lsp::test::GenerateRandomSymbolName("fnC");

    const std::string code = "void " + fnNameA + "()\n{\n    return;\n}\n\n" + "int " + fnNameB +
                             "()\n{\n    return 42;\n}\n\n" + "int@ " + fnNameC + "()\n{\n    return null;\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildControlFlowFacts(index, code);

    std::vector<const angel_lsp::analysis::ControlFlowRecord*> returns;
    for (const auto& rec : facts.records)
    {
        if (rec.kind == angel_lsp::analysis::ControlFlowRecordKind::Return)
        {
            returns.push_back(&rec);
        }
    }

    REQUIRE(returns.size() == 3);

    // fnA: bare return
    CHECK(returns[0]->returnFact.isBare);
    CHECK_FALSE(returns[0]->returnFact.hasValue);
    CHECK_FALSE(returns[0]->returnFact.returnsNullLiteral);
    CHECK(returns[0]->enclosingReturnType == "void");
    CHECK(returns[0]->enclosingFunctionName == fnNameA);

    // fnB: return 42
    CHECK_FALSE(returns[1]->returnFact.isBare);
    CHECK(returns[1]->returnFact.hasValue);
    CHECK_FALSE(returns[1]->returnFact.returnsNullLiteral);
    CHECK(returns[1]->enclosingReturnType == "int");
    CHECK(returns[1]->enclosingFunctionName == fnNameB);

    // fnC: return null
    CHECK_FALSE(returns[2]->returnFact.isBare);
    CHECK(returns[2]->returnFact.hasValue);
    CHECK(returns[2]->returnFact.returnsNullLiteral);
    CHECK(returns[2]->enclosingReturnType == "int@");
    CHECK(returns[2]->enclosingFunctionName == fnNameC);

    ts_tree_delete(tree);
}

TEST_CASE("ControlFlowFacts - Switch statements and case labels")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varVal = angel_lsp::test::GenerateRandomSymbolName("val");

    const std::string code = "void " + fnName + "()\n{\n" + "    int " + varVal + " = 1;\n" + "    switch (" + varVal +
                             ")\n" + "    {\n" + "    case 10:\n" + "        break;\n" + "    case \"invalid\":\n" +
                             "        break;\n" + "    case 3.14:\n" + "        break;\n" + "    default:\n" +
                             "        break;\n" + "    }\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildControlFlowFacts(index, code);

    const angel_lsp::analysis::ControlFlowRecord* switchRec = nullptr;
    for (const auto& rec : facts.records)
    {
        if (rec.kind == angel_lsp::analysis::ControlFlowRecordKind::Switch)
        {
            switchRec = &rec;
            break;
        }
    }

    REQUIRE(switchRec != nullptr);
    const auto labels = facts.LabelsOf(*switchRec);
    REQUIRE(labels.size() == 3);

    CHECK(labels[0].isNumberLiteral);
    CHECK_FALSE(labels[0].isUnusable);
    CHECK(labels[0].text == "10");

    CHECK(labels[1].isStringLiteral);
    CHECK(labels[1].isUnusable);

    CHECK(labels[2].isNumberLiteral);
    CHECK(labels[2].isUnusable);

    CHECK(facts.FindLabel(labels[0].bytes) == &labels[0]);

    ts_tree_delete(tree);
}

TEST_CASE("ControlFlowFacts - Empty branches and constant conditions")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");

    const std::string code =
        "void " + fnName + "()\n{\n" + "    if (true);\n" + "    else;\n" + "    while (false) {}\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildControlFlowFacts(index, code);

    const angel_lsp::analysis::ControlFlowRecord* ifRec = nullptr;
    const angel_lsp::analysis::ControlFlowRecord* whileRec = nullptr;

    for (const auto& rec : facts.records)
    {
        if (rec.kind == angel_lsp::analysis::ControlFlowRecordKind::If)
        {
            ifRec = &rec;
        }
        else if (rec.kind == angel_lsp::analysis::ControlFlowRecordKind::While)
        {
            whileRec = &rec;
        }
    }

    REQUIRE(ifRec != nullptr);
    CHECK(ifRec->branch.hasEmptyConsequence);
    CHECK(ifRec->branch.hasEmptyAlternative);
    CHECK(ifRec->condition.present);
    CHECK(ifRec->condition.isConstant);
    CHECK(ifRec->condition.constantValue);

    REQUIRE(whileRec != nullptr);
    CHECK(whileRec->condition.present);
    CHECK(whileRec->condition.isConstant);
    CHECK_FALSE(whileRec->condition.constantValue);

    ts_tree_delete(tree);
}
