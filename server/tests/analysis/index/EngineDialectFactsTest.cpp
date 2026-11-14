#include "analysis/index/EngineDialectFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarSymbols.h"

#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE("analysis/index/EngineDialectFacts")
{
    TEST_CASE("EngineDialectFacts - Foreach statements and empty list elements")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string varItem = angel_lsp::test::GenerateRandomSymbolName("item");
        const std::string varArr = angel_lsp::test::GenerateRandomSymbolName("arr");

        const std::string code = std::string("void ") + fnName + "()\n{\n" + "    foreach (auto " + varItem + " : " +
                                 varArr + ") {}\n" + "    array<int> list = {1, , 2, };\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const angel_lsp::analysis::EngineDialectFacts facts = angel_lsp::analysis::BuildEngineDialectFacts(index, code);

        REQUIRE(facts.foreachStatements.size() == 1);
        const auto& fe = facts.foreachStatements.front();
        CHECK(fe.startRow == 2);
        CHECK_FALSE(fe.bytes.Empty());
        CHECK(code.substr(fe.bytes.startByte, 7) == "foreach");

        REQUIRE(facts.emptyListElements.size() == 2);
        for (const auto& elem : facts.emptyListElements)
        {
            CHECK(elem.startRow == 3);
            CHECK(elem.endColumn == elem.startColumn + 1);
            CHECK(code.substr(elem.bytes.startByte, 1) == ",");
        }

        ts_tree_delete(tree);
    }

    TEST_CASE("EngineDialectFacts - Character literals and integer divisions")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string charVar = angel_lsp::test::GenerateRandomSymbolName("c");
        const std::string strVar = angel_lsp::test::GenerateRandomSymbolName("s");
        const std::string intVar = angel_lsp::test::GenerateRandomSymbolName("n");
        const std::string fltVar = angel_lsp::test::GenerateRandomSymbolName("f");

        const std::string code = std::string("void ") + fnName + "()\n{\n" + "    int " + charVar + " = 'A';\n" +
                                 "    string " + strVar + " = \"hello\";\n" + "    int " + intVar + " = 100 / 20;\n" +
                                 "    float " + fltVar + " = 100.0f / 20;\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const angel_lsp::analysis::EngineDialectFacts facts = angel_lsp::analysis::BuildEngineDialectFacts(index, code);

        REQUIRE(facts.charLiteralDeclarators.size() == 1);
        const auto& chDecl = facts.charLiteralDeclarators.front();
        const std::string_view typeText = std::string_view(code).substr(
            chDecl.declaredTypeRange.startByte, chDecl.declaredTypeRange.endByte - chDecl.declaredTypeRange.startByte);
        CHECK(typeText == "int");
        const std::string_view valText = std::string_view(code).substr(
            chDecl.valueSpan.bytes.startByte, chDecl.valueSpan.bytes.endByte - chDecl.valueSpan.bytes.startByte);
        CHECK(valText == "'A'");

        REQUIRE(facts.integerDivisions.size() == 1);
        const auto& divSpan = facts.integerDivisions.front();
        CHECK(divSpan.startRow == 4);
        const std::string_view divText =
            std::string_view(code).substr(divSpan.bytes.startByte, divSpan.bytes.endByte - divSpan.bytes.startByte);
        CHECK(divText == "100 / 20");

        ts_tree_delete(tree);
    }

    TEST_CASE("EngineDialectFacts - Named arguments and assignment expressions")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string targetVar = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string handleVar = angel_lsp::test::GenerateRandomSymbolName("handle");
        const std::string valVar = angel_lsp::test::GenerateRandomSymbolName("val");

        const std::string code = std::string("void ") + fnName + "()\n{\n" + "    " + fnName + "(namedParam = 42);\n" +
                                 "    " + targetVar + " = 10;\n" + "    @" + handleVar + " = " + valVar + ";\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const angel_lsp::analysis::EngineDialectFacts facts = angel_lsp::analysis::BuildEngineDialectFacts(index, code);

        REQUIRE(facts.namedArguments.size() == 1);
        const auto& namedArg = facts.namedArguments.front();
        const std::string_view nameText = std::string_view(code).substr(
            namedArg.nameRange.startByte, namedArg.nameRange.endByte - namedArg.nameRange.startByte);
        CHECK(nameText == "namedParam");
        CHECK(namedArg.argumentSpan.startRow == 2);

        REQUIRE_FALSE(facts.assignmentExpressions.empty());
        bool foundNormal = false;
        bool foundHandle = false;
        for (const auto& assign : facts.assignmentExpressions)
        {
            const std::string_view idText = std::string_view(code).substr(assign.targetIdentifierRange.startByte,
                                                                          assign.targetIdentifierRange.endByte -
                                                                              assign.targetIdentifierRange.startByte);
            if (idText == targetVar)
            {
                foundNormal = true;
            }
            else if (idText == handleVar)
            {
                foundHandle = true;
            }
        }
        CHECK(foundNormal);
        CHECK_FALSE(foundHandle);

        ts_tree_delete(tree);
    }

    TEST_CASE("EngineDialectFacts - Multiline string literal spans")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string s1 = angel_lsp::test::GenerateRandomSymbolName("s1");
        const std::string s2 = angel_lsp::test::GenerateRandomSymbolName("s2");

        const std::string code = std::string("void ") + fnName + "()\n{\n" + "    string " + s1 +
                                 " = \"multi\nline\";\n" + "    string " + s2 + " = \"\"\"heredoc\nline\"\"\";\n" +
                                 "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const angel_lsp::analysis::EngineDialectFacts facts = angel_lsp::analysis::BuildEngineDialectFacts(index, code);

        REQUIRE(facts.multilineStrings.size() == 1);
        const auto& ml = facts.multilineStrings.front();
        CHECK(ml.startRow == 2);
        CHECK(ml.endRow == 3);

        ts_tree_delete(tree);
    }

    TEST_CASE("EngineDialectFacts - Scheduler call-site extraction")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string objName = angel_lsp::test::GenerateRandomSymbolName("scheduler");
        const std::string method = angel_lsp::test::GenerateRandomSymbolName("Schedule");
        const std::string arg1 = angel_lsp::test::GenerateRandomSymbolName("task");

        const std::string code =
            std::string("void Test()\n{\n    ") + objName + "." + method + "(" + arg1 + ", 500);\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto& gs = angel_lsp::parser::GetGrammarSymbols();
        const auto callNodes = index.Nodes(gs.symCallExpression);
        REQUIRE(callNodes.size() == 1);

        const TSNode callNode = callNodes.front();
        const std::string_view rcv = angel_lsp::analysis::ExtractReceiverText(callNode, code);
        CHECK(rcv == objName);

        std::vector<TSNode> argNodes;
        const TSNode argList = angel_lsp::parser::GetChildByField(callNode, gs.fieldArguments);
        if (!ts_node_is_null(argList))
        {
            TSTreeCursor cursor = ts_tree_cursor_new(argList);
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                do
                {
                    TSNode child = ts_tree_cursor_current_node(&cursor);
                    if (ts_node_is_named(child))
                    {
                        argNodes.push_back(child);
                    }
                } while (ts_tree_cursor_goto_next_sibling(&cursor));
            }
            ts_tree_cursor_delete(&cursor);
        }

        const auto site = angel_lsp::analysis::BuildSchedulerCallSite(callNode, argNodes, code);
        CHECK(site.receiverText == objName);
        REQUIRE(site.args.size() == 2);
        CHECK(site.call.startRow == 2);

        ts_tree_delete(tree);
    }
}
