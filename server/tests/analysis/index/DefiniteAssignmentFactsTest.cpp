#include "analysis/index/DefiniteAssignmentFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("DefiniteAssignmentFacts - Function and lambda detection and body records")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string localVar = angel_lsp::test::GenerateRandomSymbolName("x");

    const std::string code =
        "void " + fnName + "()\n{\n    int " + localVar + " = 10;\n    auto l = function() { return 1; };\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildDefiniteAssignmentFacts(index, code);

    REQUIRE(facts.functions.size() >= 2);
    const angel_lsp::analysis::DefiniteAssignmentFunctionFact* fnFact = nullptr;
    const angel_lsp::analysis::DefiniteAssignmentFunctionFact* lambdaFact = nullptr;

    for (const auto& f : facts.functions)
    {
        if (!f.isLambda)
        {
            fnFact = &f;
        }
        else
        {
            lambdaFact = &f;
        }
    }

    REQUIRE(fnFact != nullptr);
    CHECK_FALSE(fnFact->isLambda);
    CHECK(fnFact->rootRecordIndex != UINT32_MAX);
    CHECK(fnFact->recordCount > 0);

    const auto* bodyRec = facts.FindRecord(fnFact->bodyBytes);
    REQUIRE(bodyRec != nullptr);
    CHECK(bodyRec->kind == angel_lsp::analysis::DefiniteAssignmentStmtKind::Block);

    const auto* foundFn = facts.FindFunction(fnFact->bytes);
    REQUIRE(foundFn != nullptr);
    CHECK(foundFn == fnFact);

    REQUIRE(lambdaFact != nullptr);
    CHECK(lambdaFact->isLambda);
    CHECK(lambdaFact->rootRecordIndex != UINT32_MAX);

    ts_tree_delete(tree);
}

TEST_CASE("DefiniteAssignmentFacts - Local variable declarations without initializer vs initialized")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string uninitPrimitive = angel_lsp::test::GenerateRandomSymbolName("uninit");
    const std::string initPrimitive = angel_lsp::test::GenerateRandomSymbolName("init");
    const std::string uninitClass = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("Cls");

    const std::string code = "class " + className + " {}\nvoid " + fnName + "()\n{\n    int " + uninitPrimitive +
                             ";\n    int " + initPrimitive + " = 42;\n    " + className + " " + uninitClass + ";\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildDefiniteAssignmentFacts(index, code);

    REQUIRE(facts.declarators.size() >= 3);
    const angel_lsp::analysis::DefiniteAssignmentLocalFact* uninitFact = nullptr;
    const angel_lsp::analysis::DefiniteAssignmentLocalFact* initFact = nullptr;
    const angel_lsp::analysis::DefiniteAssignmentLocalFact* classFact = nullptr;

    for (const auto& d : facts.declarators)
    {
        if (d.name == uninitPrimitive)
        {
            uninitFact = &d;
        }
        else if (d.name == initPrimitive)
        {
            initFact = &d;
        }
        else if (d.name == uninitClass)
        {
            classFact = &d;
        }
    }

    REQUIRE(uninitFact != nullptr);
    CHECK(uninitFact->isPrimitive);
    CHECK_FALSE(uninitFact->hasInitializer);
    CHECK(uninitFact->isTrackedUninitialized);

    REQUIRE(initFact != nullptr);
    CHECK(initFact->isPrimitive);
    CHECK(initFact->hasInitializer);
    CHECK_FALSE(initFact->isTrackedUninitialized);

    REQUIRE(classFact != nullptr);
    CHECK_FALSE(classFact->isPrimitive);
    CHECK_FALSE(classFact->hasInitializer);
    CHECK_FALSE(classFact->isTrackedUninitialized);

    const auto* foundDecl = facts.FindDeclarator(uninitFact->bytes);
    REQUIRE(foundDecl != nullptr);
    CHECK(foundDecl == uninitFact);

    ts_tree_delete(tree);
}

TEST_CASE("DefiniteAssignmentFacts - Branch and loop hierarchy with parent and child links")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varCond = angel_lsp::test::GenerateRandomSymbolName("c");

    const std::string code = "void " + fnName + "()\n{\n    bool " + varCond + " = true;\n    if (" + varCond +
                             ") {\n        return;\n    } else {\n        break;\n    }\n"
                             "    while (true) {\n        continue;\n    }\n"
                             "    for (;;) {\n        break;\n    }\n"
                             "    switch (1) {\n        case 1: break;\n        default: break;\n    }\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildDefiniteAssignmentFacts(index, code);

    bool foundIf = false;
    bool foundWhile = false;
    bool foundFor = false;
    bool foundSwitch = false;
    bool foundDefaultCase = false;

    for (const auto& rec : facts.records)
    {
        if (rec.kind == angel_lsp::analysis::DefiniteAssignmentStmtKind::If)
        {
            foundIf = true;
            const auto children = facts.ChildrenOf(rec);
            CHECK(children.size() >= 2);
        }
        else if (rec.kind == angel_lsp::analysis::DefiniteAssignmentStmtKind::While)
        {
            foundWhile = true;
            CHECK(rec.conditionAlwaysTrue);
        }
        else if (rec.kind == angel_lsp::analysis::DefiniteAssignmentStmtKind::For)
        {
            foundFor = true;
            CHECK(rec.conditionAlwaysTrue);
        }
        else if (rec.kind == angel_lsp::analysis::DefiniteAssignmentStmtKind::Switch)
        {
            foundSwitch = true;
            const auto cases = facts.ChildrenOf(rec);
            CHECK(cases.size() >= 2);
        }
        else if (rec.kind == angel_lsp::analysis::DefiniteAssignmentStmtKind::Case && rec.isDefaultCase)
        {
            foundDefaultCase = true;
        }
    }

    CHECK(foundIf);
    CHECK(foundWhile);
    CHECK(foundFor);
    CHECK(foundSwitch);
    CHECK(foundDefaultCase);

    ts_tree_delete(tree);
}

TEST_CASE("DefiniteAssignmentFacts - Identifier reads and assignment writes")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");
    const std::string varC = angel_lsp::test::GenerateRandomSymbolName("c");

    const std::string code = "void " + fnName + "()\n{\n    int " + varA + ";\n    int " + varB + " = 1;\n    int " +
                             varC + " = 2;\n    " + varA + " = " + varB + " + " + varC + ";\n    " + varA +
                             " += " + varB + ";\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildDefiniteAssignmentFacts(index, code);

    REQUIRE(facts.assignments.size() >= 2);
    const auto* assign0 = facts.FindAssignment(facts.assignments[0].bytes);
    REQUIRE(assign0 != nullptr);
    CHECK(assign0->op == "=");
    CHECK(assign0->opIsAssign);
    CHECK(assign0->left.foldedName == varA);

    const auto* assign1 = facts.FindAssignment(facts.assignments[1].bytes);
    REQUIRE(assign1 != nullptr);
    CHECK(assign1->op == "+=");
    CHECK_FALSE(assign1->opIsAssign);
    CHECK(assign1->left.foldedName == varA);

    bool foundReadB = false;
    bool foundReadC = false;
    for (const auto& read : facts.reads)
    {
        if (read.name == varB)
            foundReadB = true;
        if (read.name == varC)
            foundReadC = true;
    }
    CHECK(foundReadB);
    CHECK(foundReadC);

    ts_tree_delete(tree);
}

TEST_CASE("DefiniteAssignmentFacts - Call expressions and arguments")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string calleeName = angel_lsp::test::GenerateRandomSymbolName("callee");
    const std::string objName = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string memName = angel_lsp::test::GenerateRandomSymbolName("method");
    const std::string argX = angel_lsp::test::GenerateRandomSymbolName("x");
    const std::string argY = angel_lsp::test::GenerateRandomSymbolName("y");

    const std::string code = "void " + fnName + "()\n{\n    int " + argX + ";\n    int " + argY + ";\n    " +
                             calleeName + "(" + argX + ", " + argY + ");\n    " + objName + "." + memName + "(" + argX +
                             ");\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildDefiniteAssignmentFacts(index, code);

    REQUIRE(facts.calls.size() >= 2);
    const angel_lsp::analysis::DefiniteAssignmentCallFact* freeCall = nullptr;
    const angel_lsp::analysis::DefiniteAssignmentCallFact* memberCall = nullptr;

    for (const auto& c : facts.calls)
    {
        if (!c.isMemberCall)
            freeCall = &c;
        else
            memberCall = &c;
    }

    REQUIRE(freeCall != nullptr);
    CHECK(freeCall->calleeText == calleeName);
    const auto freeArgs = facts.ArgumentsOf(*freeCall);
    REQUIRE(freeArgs.size() == 2);
    CHECK(freeArgs[0].name == argX);
    CHECK(freeArgs[0].isSimpleIdentifier);
    CHECK(freeArgs[1].name == argY);
    CHECK(freeArgs[1].isSimpleIdentifier);

    REQUIRE(memberCall != nullptr);
    CHECK(memberCall->isMemberCall);
    CHECK(memberCall->memberName == memName);
    CHECK(memberCall->objectText == objName);
    const auto memArgs = facts.ArgumentsOf(*memberCall);
    REQUIRE(memArgs.size() == 1);
    CHECK(memArgs[0].name == argX);

    const auto* foundCall = facts.FindCall(freeCall->bytes);
    REQUIRE(foundCall != nullptr);
    CHECK(foundCall == freeCall);

    ts_tree_delete(tree);
}
