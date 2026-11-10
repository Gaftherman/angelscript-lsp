#include "analysis/index/InitializerListFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("InitializerListFacts - Variable declarations and element kinds")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Type");
    const std::string varArr = angel_lsp::test::GenerateRandomSymbolName("arr");
    const std::string varElem = angel_lsp::test::GenerateRandomSymbolName("elem");
    const std::string callFn = angel_lsp::test::GenerateRandomSymbolName("calc");

    const std::string code = "void " + fnName + "()\n{\n    int " + varElem + " = 10;\n    " + typeName + " " + varArr +
                             " = { 42, " + varElem + ", " + callFn + "(), { 1, 2 } };\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildInitializerListFacts(index, code);

    REQUIRE(facts.lists.size() == 2);

    const auto& outerList = facts.lists[0];
    CHECK(outerList.parentListIndex == angel_lsp::analysis::NoInitializerList);
    CHECK(outerList.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::VariableDeclaration);
    CHECK(outerList.owner.targetType == typeName);
    CHECK(outerList.elementCount == 4);
    CHECK(outerList.valueCount == 4);

    const auto elems = facts.Elements(outerList);
    REQUIRE(elems.size() == 4);

    CHECK(elems[0].kind == angel_lsp::analysis::InitializerListElementKind::Literal);
    CHECK(elems[0].text == "42");
    CHECK(elems[0].value.kind == angel_lsp::analysis::ValueExpressionKind::Number);

    CHECK(elems[1].kind == angel_lsp::analysis::InitializerListElementKind::Identifier);
    CHECK(elems[1].text == varElem);
    CHECK(elems[1].value.kind == angel_lsp::analysis::ValueExpressionKind::Identifier);

    CHECK(elems[2].kind == angel_lsp::analysis::InitializerListElementKind::Call);
    CHECK(elems[2].value.kind == angel_lsp::analysis::ValueExpressionKind::Call);

    CHECK(elems[3].kind == angel_lsp::analysis::InitializerListElementKind::NestedList);
    CHECK(elems[3].nestedListIndex == 1);

    const auto& innerList = facts.lists[1];
    CHECK(innerList.parentListIndex == 0);
    CHECK(innerList.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::ParentList);
    CHECK(innerList.elementCount == 2);
    CHECK(innerList.valueCount == 2);

    const auto* lookupOuter = facts.Find(outerList.bytes);
    REQUIRE(lookupOuter != nullptr);
    CHECK(lookupOuter->bytes.startByte == outerList.bytes.startByte);

    const auto* lookupInner = facts.FindList(innerList.bytes);
    REQUIRE(lookupInner != nullptr);
    CHECK(lookupInner->bytes.startByte == innerList.bytes.startByte);

    ts_tree_delete(tree);
}

TEST_CASE("InitializerListFacts - Typed initializer lists and anonymous objects")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Point");

    const std::string listType = "array<" + typeName + ">";
    const std::string code = "void " + fnName + "()\n{\n    " + listType + " a = " + listType + " = { 10, 20 };\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildInitializerListFacts(index, code);

    REQUIRE(facts.lists.size() == 1);
    const auto& list = facts.lists[0];
    CHECK(list.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::TypedList);
    CHECK(list.owner.targetType == listType);
    CHECK(list.elementCount == 2);
    CHECK(list.valueCount == 2);

    ts_tree_delete(tree);
}

TEST_CASE("InitializerListFacts - Assignment expressions and associative subscripts")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varObj = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string varDict = angel_lsp::test::GenerateRandomSymbolName("dict");

    const std::string code =
        "void " + fnName + "()\n{\n    " + varObj + " = { 1, 2 };\n    " + varDict + "[\"entry\"] = { 3, 4 };\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildInitializerListFacts(index, code);

    REQUIRE(facts.lists.size() == 2);

    const auto& plainAssign = facts.lists[0];
    CHECK(plainAssign.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::Assignment);
    CHECK_FALSE(plainAssign.owner.isSubscriptAssignment);
    CHECK_FALSE(plainAssign.owner.hasStringIndex);

    const auto& dictAssign = facts.lists[1];
    CHECK(dictAssign.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::Assignment);
    CHECK(dictAssign.owner.isSubscriptAssignment);
    CHECK(dictAssign.owner.hasStringIndex);

    ts_tree_delete(tree);
}

TEST_CASE("InitializerListFacts - Return statements inside functions and lambdas")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string retTypeName = angel_lsp::test::GenerateRandomSymbolName("Vec");
    const std::string lambdaVar = angel_lsp::test::GenerateRandomSymbolName("cb");

    const std::string code = retTypeName + " " + fnName + "()\n{\n    auto " + lambdaVar +
                             " = function() { return { 3, 4 }; };\n    return { 1, 2 };\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildInitializerListFacts(index, code);

    REQUIRE(facts.lists.size() == 2);

    // Lists are ordered by source byte range. The lambda return sits earlier in the source code.
    const auto& lambdaReturn = facts.lists[0];
    CHECK(lambdaReturn.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::ReturnStatement);
    CHECK(lambdaReturn.owner.targetType.empty());

    const auto& funcReturn = facts.lists[1];
    CHECK(funcReturn.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::ReturnStatement);
    CHECK(funcReturn.owner.targetType == retTypeName);

    ts_tree_delete(tree);
}

TEST_CASE("InitializerListFacts - Call arguments and construct calls")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string calleeName = angel_lsp::test::GenerateRandomSymbolName("process");
    const std::string constructName = angel_lsp::test::GenerateRandomSymbolName("Holder");

    const std::string code = "void " + fnName + "()\n{\n    " + calleeName + "(10, { 1, 2 });\n    " + constructName +
                             "<int>({ 3, 4 });\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildInitializerListFacts(index, code);

    REQUIRE(facts.lists.size() == 2);

    const auto& callArg = facts.lists[0];
    CHECK(callArg.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::CallArgument);
    CHECK(callArg.owner.argumentIndex == 1);
    CHECK(callArg.owner.targetType.empty());

    const auto& constructArg = facts.lists[1];
    CHECK(constructArg.owner.kind == angel_lsp::analysis::InitializerListOwnerKind::CallArgument);
    CHECK(constructArg.owner.argumentIndex == 0);
    CHECK(constructArg.owner.targetType.starts_with(constructName));

    ts_tree_delete(tree);
}

TEST_CASE("InitializerListFacts - Comma separators and comments")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("listA");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("listB");

    const std::string code = "void " + fnName + "()\n{\n    auto " + varA + " = { 1, , 3 };\n    auto " + varB +
                             " = { /* comment */ 4, 5 };\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildInitializerListFacts(index, code);

    REQUIRE(facts.lists.size() == 2);

    const auto& omittedList = facts.lists[0];
    CHECK(omittedList.elementCount == 2);
    CHECK(omittedList.valueCount == 3);

    const auto& commentList = facts.lists[1];
    CHECK(commentList.elementCount == 2);
    CHECK(commentList.valueCount == 2);

    ts_tree_delete(tree);
}

TEST_CASE("InitializerListFacts - Multi-dimensional container nesting")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string varMatrix = angel_lsp::test::GenerateRandomSymbolName("matrix");

    const std::string code = "void " + fnName + "()\n{\n    auto " + varMatrix + " = { { 1, 2 }, { 3, 4 } };\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildInitializerListFacts(index, code);

    REQUIRE(facts.lists.size() == 3);

    const auto& outer = facts.lists[0];
    CHECK(outer.parentListIndex == angel_lsp::analysis::NoInitializerList);
    CHECK(outer.elementCount == 2);

    const auto outerElems = facts.Elements(outer);
    REQUIRE(outerElems.size() == 2);
    CHECK(outerElems[0].kind == angel_lsp::analysis::InitializerListElementKind::NestedList);
    CHECK(outerElems[0].nestedListIndex == 1);
    CHECK(outerElems[1].kind == angel_lsp::analysis::InitializerListElementKind::NestedList);
    CHECK(outerElems[1].nestedListIndex == 2);

    const auto& row0 = facts.lists[1];
    CHECK(row0.parentListIndex == 0);
    CHECK(row0.elementCount == 2);

    const auto& row1 = facts.lists[2];
    CHECK(row1.parentListIndex == 0);
    CHECK(row1.elementCount == 2);

    ts_tree_delete(tree);
}
