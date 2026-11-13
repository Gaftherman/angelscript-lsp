#include "analysis/index/HoverFacts.h"

#include "analysis/index/NodeIndex.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <ostream>
#include <string>
#include <string_view>

TEST_CASE("HoverFacts - Target and parent range on a plain identifier")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("myVar");
    const std::string code = "int " + varName + " = 42;\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
    const size_t pos = code.find(varName);
    REQUIRE(pos != std::string::npos);

    const auto site = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 0, static_cast<uint32_t>(pos));
    REQUIRE(site.has_value());
    CHECK(site->kind == angel_lsp::analysis::HoverNodeKind::Identifier);
    CHECK(site->targetStartByte == static_cast<uint32_t>(pos));
    CHECK(site->targetEndByte == static_cast<uint32_t>(pos + varName.size()));
    CHECK(site->parentStartByte <= site->targetStartByte);
    CHECK(site->parentEndByte >= site->targetEndByte);

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Member access with receiver range")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string objName = angel_lsp::test::GenerateRandomSymbolName("player");
    const std::string memName = angel_lsp::test::GenerateRandomSymbolName("health");
    const std::string code = "void test() { " + objName + "." + memName + " = 100; }\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
    const size_t memPos = code.find(memName);
    REQUIRE(memPos != std::string::npos);

    const auto site = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 0, static_cast<uint32_t>(memPos));
    REQUIRE(site.has_value());
    CHECK(site->kind == angel_lsp::analysis::HoverNodeKind::MemberAccess);
    CHECK(site->isMemberOfExpression == 1);
    CHECK(site->isQualifiedMember == 1);
    CHECK(!site->receiverRange.Empty());

    const size_t objPos = code.find(objName);
    CHECK(site->receiverRange.startByte == static_cast<uint32_t>(objPos));
    CHECK(site->receiverRange.endByte == static_cast<uint32_t>(objPos + objName.size()));

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Scoped member qualifier")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("MyNamespace");
    const std::string memName = angel_lsp::test::GenerateRandomSymbolName("DoWork");
    const std::string code = "void test() { " + nsName + "::" + memName + "(); }\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
    const size_t nsPos = code.find(nsName);
    REQUIRE(nsPos != std::string::npos);

    const auto site = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 0, static_cast<uint32_t>(nsPos));
    REQUIRE(site.has_value());
    CHECK(site->kind == angel_lsp::analysis::HoverNodeKind::ScopedMember);
    CHECK(site->isScopeQualifier == 1);
    CHECK(!site->nextScopedMemberRange.Empty());

    const size_t memPos = code.find(memName);
    CHECK(site->nextScopedMemberRange.startByte == static_cast<uint32_t>(memPos));
    CHECK(site->nextScopedMemberRange.endByte == static_cast<uint32_t>(memPos + memName.size()));

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Call and argument list range")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("computeSum");
    const std::string code = "void test() { " + fnName + "(10, 20); }\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
    const size_t fnPos = code.find(fnName);
    REQUIRE(fnPos != std::string::npos);

    const auto site = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 0, static_cast<uint32_t>(fnPos));
    REQUIRE(site.has_value());
    CHECK(!site->enclosingCallRange.Empty());
    CHECK(!site->argumentListRange.Empty());

    const size_t argPos = code.find("(10, 20)");
    CHECK(site->argumentListRange.startByte == static_cast<uint32_t>(argPos));
    CHECK(site->argumentListRange.endByte == static_cast<uint32_t>(argPos + 8));

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Direct-init declarator")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("CustomVec");
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("vec");
    const std::string code = "void test() { " + clsName + " " + varName + "(1.0f, 2.0f); }\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
    const size_t varPos = code.find(varName);
    REQUIRE(varPos != std::string::npos);

    const auto site = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 0, static_cast<uint32_t>(varPos));
    REQUIRE(site.has_value());
    CHECK(site->isDirectInitDeclarator == 1);
    CHECK(!site->directInitDeclaratorRange.Empty());
    CHECK(!site->argumentListRange.Empty());

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Lambda and auto enclosing ranges")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("val");
    const std::string code = "void test() {\n"
                             "    auto " +
                             varName +
                             " = 10;\n"
                             "    auto fn = function() {};\n"
                             "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});

    // Test auto
    const size_t autoPos = code.find("auto " + varName);
    REQUIRE(autoPos != std::string::npos);
    const auto autoSite = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 1, 4);
    REQUIRE(autoSite.has_value());
    CHECK(autoSite->kind == angel_lsp::analysis::HoverNodeKind::Auto);
    CHECK(!autoSite->enclosingAutoDeclRange.Empty());

    // Test lambda
    const size_t fnPos = code.find("function()");
    REQUIRE(fnPos != std::string::npos);
    const auto lambdaSite = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 2, 14);
    REQUIRE(lambdaSite.has_value());
    CHECK(lambdaSite->kind == angel_lsp::analysis::HoverNodeKind::Lambda);
    CHECK(!lambdaSite->enclosingLambdaRange.Empty());

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Named argument label")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("setConfig");
    const std::string labelName = "timeout";
    const std::string code = "void test() { " + fnName + "(" + labelName + ": 5000); }\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
    const size_t lblPos = code.find(labelName);
    REQUIRE(lblPos != std::string::npos);

    const auto site = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 0, static_cast<uint32_t>(lblPos));
    REQUIRE(site.has_value());
    CHECK(site->kind == angel_lsp::analysis::HoverNodeKind::NamedArgument);
    CHECK(site->isNamedArgumentLabel == 1);

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Column-1 fallback when cursor just after identifier")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string idName = angel_lsp::test::GenerateRandomSymbolName("targetSymbol");
    const std::string code = "int " + idName + " = 1;\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
    const size_t idEnd = code.find(idName) + idName.size();

    const auto site = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 0, static_cast<uint32_t>(idEnd));
    REQUIRE(site.has_value());
    CHECK(site->targetEndByte == static_cast<uint32_t>(idEnd));

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Include directive path bytes")
{
    const std::string incPath = "scripts/common/math.as";
    const std::string code = "#include \"" + incPath + "\"\nvoid main() {}\n";

    const auto directive = angel_lsp::analysis::FindIncludeDirective(code, 0);
    REQUIRE(directive.has_value());
    CHECK(directive->hash == 0);
    CHECK(directive->closeQuote == code.find('"', code.find('"') + 1));
    const std::string_view pathSlice =
        std::string_view(code).substr(directive->pathStartByte, directive->pathEndByte - directive->pathStartByte);
    CHECK(pathSlice == incPath);
}

TEST_CASE("HoverFacts - No site on whitespace")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string code = "   \n\n   int a = 1;\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto site = angel_lsp::analysis::FindHoverSite(nodeIndex, code, 0, 1);
    CHECK(!site.has_value());

    ts_tree_delete(tree);
}

TEST_CASE("HoverFacts - Namespace-qualified datatype is a qualified member")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("ns");
    const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Widget");
    const std::string code =
        "namespace " + nsName + " { class " + typeName + " {} }\nvoid f() { " + nsName + "::" + typeName + " w; }\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
    const size_t lineStart = code.find('\n') + 1;
    const size_t nsPos = code.find(nsName + "::");
    REQUIRE(nsPos != std::string::npos);
    const size_t typePos = code.find(typeName, nsPos);

    const auto member =
        angel_lsp::analysis::FindHoverSite(nodeIndex, code, 1, static_cast<uint32_t>(typePos - lineStart));
    REQUIRE(member.has_value());
    CHECK(member->isScopeQualifier == 0);
    CHECK(member->isQualifiedMember == 1);

    ts_tree_delete(tree);
}
