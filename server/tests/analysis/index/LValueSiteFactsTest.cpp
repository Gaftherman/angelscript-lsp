#include "analysis/index/LValueSiteFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE("analysis/index/LValueSiteFacts")
{
    TEST_CASE("LValueSiteFacts - Assignment targets and identifier candidates")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
        const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");

        const std::string code = "void " + fnName + "()\n{\n    " + varA + " = " + varB + ";\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildLValueSiteFacts(index, code);

        REQUIRE(facts.assignments.size() == 1);
        const auto& assign = facts.assignments.front();

        CHECK(assign.targetText == varA);
        CHECK(assign.targetCandidate.kind == angel_lsp::analysis::LValueCandidateKind::Identifier);
        CHECK(assign.targetCandidate.name == varA);
        CHECK_FALSE(assign.targetIsDirectCall);
        CHECK_FALSE(assign.targetIsUnwrappedCall);
        CHECK_FALSE(assign.targetIsDirectTernary);

        const auto* foundAssign = facts.FindAssignment(assign.bytes);
        REQUIRE(foundAssign != nullptr);
        CHECK(foundAssign == &assign);

        const auto* foundCand = facts.FindCandidate(assign.targetBytes);
        REQUIRE(foundCand != nullptr);
        CHECK(foundCand->name == varA);
        CHECK(foundCand->kind == angel_lsp::analysis::LValueCandidateKind::Identifier);

        ts_tree_delete(tree);
    }

    TEST_CASE("LValueSiteFacts - Call sites used as lvalues (identifier, member, scoped)")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string callerFn = angel_lsp::test::GenerateRandomSymbolName("caller");
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("callee");
        const std::string objName = angel_lsp::test::GenerateRandomSymbolName("obj");
        const std::string memName = angel_lsp::test::GenerateRandomSymbolName("mem");
        const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("ns");
        const std::string scopedFunc = angel_lsp::test::GenerateRandomSymbolName("func");

        const std::string code = "void " + callerFn + "()\n{\n" + "    " + fnName + "() = 1;\n" + "    " + objName +
                                 "." + memName + "() = 2;\n" + "    " + nsName + "::" + scopedFunc + "() = 3;\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildLValueSiteFacts(index, code);

        REQUIRE(facts.assignments.size() == 3);
        REQUIRE(facts.callSites.size() == 3);

        const auto* call0 = facts.FindCallSite(facts.assignments[0].callBytes);
        REQUIRE(call0 != nullptr);
        // The grammar wraps every plain name in scoped_identifier, so it classifies as Scoped.
        CHECK(call0->calleeKind == angel_lsp::analysis::LValueCalleeKind::Scoped);
        CHECK(call0->calleeText == fnName);

        const auto* call1 = facts.FindCallSite(facts.assignments[1].callBytes);
        REQUIRE(call1 != nullptr);
        CHECK(call1->calleeKind == angel_lsp::analysis::LValueCalleeKind::Member);
        CHECK(call1->memberName == memName);
        CHECK(call1->objectText == objName);

        const auto* call2 = facts.FindCallSite(facts.assignments[2].callBytes);
        REQUIRE(call2 != nullptr);
        CHECK(call2->calleeKind == angel_lsp::analysis::LValueCalleeKind::Scoped);
        CHECK(call2->calleeText == nsName + "::" + scopedFunc);

        ts_tree_delete(tree);
    }

    TEST_CASE("LValueSiteFacts - Parenthesized targets and call unwrapping")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string callerFn = angel_lsp::test::GenerateRandomSymbolName("caller");
        const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");

        const std::string code =
            "void " + callerFn + "()\n{\n    ((" + varA + ")) = 10;\n    (" + fnName + "()) = 20;\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildLValueSiteFacts(index, code);

        REQUIRE(facts.assignments.size() == 2);

        const auto& assign0 = facts.assignments[0];
        CHECK_FALSE(assign0.targetIsDirectCall);
        CHECK_FALSE(assign0.targetIsUnwrappedCall);
        CHECK(assign0.targetCandidate.isParenthesized);
        CHECK(assign0.targetCandidate.name == varA);
        CHECK(assign0.targetCandidate.unwrappedBytes.startByte > assign0.targetBytes.startByte);

        const auto& assign1 = facts.assignments[1];
        CHECK_FALSE(assign1.targetIsDirectCall);
        CHECK(assign1.targetIsUnwrappedCall);
        const auto* callFact = facts.FindCallSite(assign1.callBytes);
        REQUIRE(callFact != nullptr);
        CHECK(callFact->calleeText == fnName);

        ts_tree_delete(tree);
    }

    TEST_CASE("LValueSiteFacts - Member, index, and unary handle dereference candidates")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string callerFn = angel_lsp::test::GenerateRandomSymbolName("caller");
        const std::string objName = angel_lsp::test::GenerateRandomSymbolName("obj");
        const std::string propName = angel_lsp::test::GenerateRandomSymbolName("prop");
        const std::string arrName = angel_lsp::test::GenerateRandomSymbolName("arr");
        const std::string idxName = angel_lsp::test::GenerateRandomSymbolName("idx");
        const std::string hName = angel_lsp::test::GenerateRandomSymbolName("h");
        const std::string otherName = angel_lsp::test::GenerateRandomSymbolName("other");

        const std::string code = "void " + callerFn + "()\n{\n" + "    " + objName + "." + propName + " = 1;\n" +
                                 "    " + arrName + "[" + idxName + "] = 2;\n" + "    @" + hName + " = @" + otherName +
                                 ";\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildLValueSiteFacts(index, code);

        REQUIRE(facts.assignments.size() == 3);

        const auto& assign0 = facts.assignments[0];
        CHECK(assign0.targetCandidate.kind == angel_lsp::analysis::LValueCandidateKind::Member);
        CHECK(assign0.targetCandidate.memberName == propName);
        CHECK(assign0.targetCandidate.objectText == objName);

        const auto& assign1 = facts.assignments[1];
        CHECK(assign1.targetCandidate.kind == angel_lsp::analysis::LValueCandidateKind::Index);

        const auto& assign2 = facts.assignments[2];
        CHECK(assign2.targetCandidate.kind == angel_lsp::analysis::LValueCandidateKind::UnaryDereference);
        CHECK(assign2.targetCandidate.operandText == hName);

        const auto* candH = facts.FindCandidate(assign2.targetCandidate.operandBytes);
        REQUIRE(candH != nullptr);
        CHECK(candH->kind == angel_lsp::analysis::LValueCandidateKind::Identifier);
        CHECK(candH->name == hName);

        ts_tree_delete(tree);
    }

    TEST_CASE("LValueSiteFacts - Ternary conditional candidate with flattened branches")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string callerFn = angel_lsp::test::GenerateRandomSymbolName("caller");
        const std::string condVar = angel_lsp::test::GenerateRandomSymbolName("cond");
        const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
        const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");

        const std::string code =
            "void " + callerFn + "()\n{\n    (" + condVar + " ? " + varA + " : " + varB + ") = 1;\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildLValueSiteFacts(index, code);

        REQUIRE(facts.assignments.size() == 1);
        const auto& cand = facts.assignments[0].targetCandidate;
        CHECK(cand.kind == angel_lsp::analysis::LValueCandidateKind::Ternary);
        CHECK(cand.consequenceText == varA);
        CHECK(cand.alternativeText == varB);
        CHECK(cand.branchCount == 2);
        REQUIRE(cand.branchBegin + cand.branchCount <= facts.ternaryBranches.size());

        CHECK(facts.ternaryBranches[cand.branchBegin].isConsequence);
        CHECK(facts.ternaryBranches[cand.branchBegin].text == varA);
        CHECK_FALSE(facts.ternaryBranches[cand.branchBegin + 1].isConsequence);
        CHECK(facts.ternaryBranches[cand.branchBegin + 1].text == varB);

        const auto* branchA = facts.FindCandidate(cand.consequenceBytes);
        REQUIRE(branchA != nullptr);
        CHECK(branchA->name == varA);

        const auto* branchB = facts.FindCandidate(cand.alternativeBytes);
        REQUIRE(branchB != nullptr);
        CHECK(branchB->name == varB);

        ts_tree_delete(tree);
    }

    TEST_CASE("LValueSiteFacts - Exact span lookup edge cases")
    {
        angel_lsp::analysis::LValueSiteFacts emptyFacts;
        CHECK(emptyFacts.FindAssignment({0, 0}) == nullptr);
        CHECK(emptyFacts.FindCallSite({0, 0}) == nullptr);
        CHECK(emptyFacts.FindCandidate({0, 0}) == nullptr);
    }
}
