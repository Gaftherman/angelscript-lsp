#include "analysis/index/EnumDeclarationFacts.h"
#include "analysis/index/MixinSourceIndex.h"
#include "analysis/index/ReflectionCallFacts.h"
#include "analysis/index/RuleSyntaxFacts.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE("analysis/index/RuleFacts")
{
    TEST_CASE("ReflectionCallFacts - Concatenated literals and non-literal argument")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("caller");
        const std::string calleeName = angel_lsp::test::GenerateRandomSymbolName("callee");
        const std::string nonLitVar = angel_lsp::test::GenerateRandomSymbolName("num");

        const std::string code = "void " + fnName + "(int " + nonLitVar + ") {\n" + "    " + calleeName +
                                 "(\"hello \" + \"world\", " + nonLitVar + ", \"test\");\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto calls = angel_lsp::analysis::BuildReflectionCallFacts(index, code);
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].calleeText == calleeName);
        REQUIRE(calls[0].argStringValues.size() == 3);
        CHECK(calls[0].argStringValues[0] == "hello world");
        CHECK(calls[0].argStringValues[1].empty());
        CHECK(calls[0].argStringValues[2] == "test");

        ts_tree_delete(tree);
    }

    TEST_CASE("EnumDeclarationFacts - Enums with zero, one, and duplicate members")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string enumEmpty = angel_lsp::test::GenerateRandomSymbolName("EnumEmpty");
        const std::string enumSingle = angel_lsp::test::GenerateRandomSymbolName("EnumSingle");
        const std::string memberSingle = angel_lsp::test::GenerateRandomSymbolName("MemberSingle");
        const std::string enumDup = angel_lsp::test::GenerateRandomSymbolName("EnumDup");
        const std::string memberDup = angel_lsp::test::GenerateRandomSymbolName("MemberDup");
        const std::string memberOther = angel_lsp::test::GenerateRandomSymbolName("MemberOther");

        const std::string code = "enum " + enumEmpty + " {}\n" + "enum " + enumSingle + " {\n    " + memberSingle +
                                 "\n}\n" + "enum " + enumDup + " {\n    " + memberDup + ",\n    " + memberOther +
                                 ",\n    " + memberDup + "\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto enums = angel_lsp::analysis::BuildEnumDeclarationFacts(index, code);
        REQUIRE(enums.size() == 3);

        CHECK(enums[0].name == enumEmpty);
        CHECK(enums[0].start.line == 0);
        CHECK(enums[0].members.empty());

        CHECK(enums[1].name == enumSingle);
        CHECK(enums[1].start.line == 1);
        REQUIRE(enums[1].members.size() == 1);
        CHECK(enums[1].members[0].name == memberSingle);
        CHECK(code.substr(enums[1].members[0].span.bytes.startByte,
                          enums[1].members[0].span.bytes.endByte - enums[1].members[0].span.bytes.startByte) ==
              memberSingle);

        CHECK(enums[2].name == enumDup);
        CHECK(enums[2].start.line == 4);
        REQUIRE(enums[2].members.size() == 3);
        CHECK(enums[2].members[0].name == memberDup);
        CHECK(enums[2].members[1].name == memberOther);
        CHECK(enums[2].members[2].name == memberDup);

        ts_tree_delete(tree);
    }

    TEST_CASE("RuleSyntaxFacts - Standalone lambda statement versus argument lambda")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string takeName = angel_lsp::test::GenerateRandomSymbolName("take");
        const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("x");

        const std::string code = "void " + fnName + "() {\n" + "    function() {};\n" + "    " + takeName +
                                 "(function(int " + paramName + ") {});\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildRuleSyntaxFacts(index);
        REQUIRE(facts.standaloneLambdas.size() == 1);
        const auto& span = facts.standaloneLambdas[0];
        CHECK_FALSE(span.bytes.Empty());
        CHECK(code.substr(span.bytes.startByte, 8) == "function");

        ts_tree_delete(tree);
    }

    TEST_CASE("RuleSyntaxFacts - Metadata attribute extraction")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string attrName = angel_lsp::test::GenerateRandomSymbolName("attr");
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");

        const std::string code = "[" + attrName + "]\nvoid " + fnName + "() {}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildRuleSyntaxFacts(index);
        REQUIRE(facts.metadata.size() == 1);
        const auto& metaSpan = facts.metadata[0];
        const std::string_view metaText =
            std::string_view(code).substr(metaSpan.bytes.startByte, metaSpan.bytes.endByte - metaSpan.bytes.startByte);
        CHECK(metaText.find(attrName) != std::string_view::npos);

        ts_tree_delete(tree);
    }

    TEST_CASE("RuleSyntaxFacts - TreeHasSyntaxError detects valid and broken syntax")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string validFn = angel_lsp::test::GenerateRandomSymbolName("validFn");
        const std::string brokenFn = angel_lsp::test::GenerateRandomSymbolName("brokenFn");

        const std::string validCode = "void " + validFn + "() {}\n";
        TSTree* validTree = parser.Parse(validCode);
        REQUIRE(validTree != nullptr);
        CHECK_FALSE(angel_lsp::analysis::TreeHasSyntaxError(validTree));
        ts_tree_delete(validTree);

        const std::string brokenCode = "void " + brokenFn + "( {\n";
        TSTree* brokenTree = parser.Parse(brokenCode);
        REQUIRE(brokenTree != nullptr);
        CHECK(angel_lsp::analysis::TreeHasSyntaxError(brokenTree));
        ts_tree_delete(brokenTree);
    }
}
