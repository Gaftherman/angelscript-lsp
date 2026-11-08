#include "analysis/index/CodeActionFacts.h"

#include "document/Document.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/Utils.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <string_view>

TEST_CASE("CodeActionFacts - node span at point")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fn = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string v = angel_lsp::test::GenerateRandomSymbolName("v");
    const std::string code = "void " + fn + "() { int " + v + " = 1; }";
    angel_lsp::document::TreePtr tree = angel_lsp::document::MakeTreePtr(parser.Parse(code));
    angel_lsp::test::NodeIndexFixture fx(tree.get());

    const auto vCol = static_cast<uint32_t>(code.find(v));
    const auto vEndCol = static_cast<uint32_t>(vCol + v.size());
    const auto span = angel_lsp::analysis::FindNodeSpanAtPoint(fx.index, 0, vCol);
    REQUIRE(span.has_value());
    const std::string_view spanText = angel_lsp::utils::SliceSource(code, span->bytes.startByte, span->bytes.endByte);
    CHECK(std::string(spanText) == v);
    CHECK(span->range.startLine == 0);
    CHECK(span->range.endLine == 0);
    CHECK(span->range.startCharacter == vCol);
    CHECK(span->range.endCharacter == vEndCol);

    angel_lsp::test::NodeIndexFixture empty(nullptr);
    CHECK_FALSE(angel_lsp::analysis::FindNodeSpanAtPoint(empty.index, 0, vCol).has_value());
}

TEST_CASE("CodeActionFacts - unused declarator removal")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string b = angel_lsp::test::GenerateRandomSymbolName("b");
    const std::string c = angel_lsp::test::GenerateRandomSymbolName("c");

    const std::string singleCode = "void f() {\n  int " + a + " = 1;\n}";
    angel_lsp::document::TreePtr singleTree = angel_lsp::document::MakeTreePtr(parser.Parse(singleCode));
    angel_lsp::test::NodeIndexFixture singleFx(singleTree.get());

    const auto singleLine1Start = singleCode.find('\n') + 1;
    const auto singleACol = static_cast<uint32_t>(singleCode.find(a) - singleLine1Start);
    const auto singleResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(singleFx.index, 1, singleACol);
    CHECK(singleResult.inDeclaration);
    CHECK(singleResult.declaratorCount == 1);

    const std::string multiCode = "void f() {\n  int " + a + " = 1, " + b + " = 2, " + c + " = 3;\n}";
    angel_lsp::document::TreePtr multiTree = angel_lsp::document::MakeTreePtr(parser.Parse(multiCode));
    angel_lsp::test::NodeIndexFixture multiFx(multiTree.get());

    const auto multiLine1Start = multiCode.find('\n') + 1;
    const auto multiACol = static_cast<uint32_t>(multiCode.find(a) - multiLine1Start);
    const auto multiBCol = static_cast<uint32_t>(multiCode.find(b) - multiLine1Start);

    const std::string declA = a + " = 1";
    const std::string declB = b + " = 2";
    const auto declAStartCol = static_cast<uint32_t>(multiCode.find(declA) - multiLine1Start);
    const auto declBStartCol = static_cast<uint32_t>(multiCode.find(declB) - multiLine1Start);
    const auto declAEndCol = static_cast<uint32_t>(multiCode.find(declA) + declA.size() - multiLine1Start);
    const auto declBEndCol = static_cast<uint32_t>(multiCode.find(declB) + declB.size() - multiLine1Start);

    const auto aResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(multiFx.index, 1, multiACol);
    CHECK(aResult.inDeclaration);
    CHECK(aResult.declaratorCount == 3);
    CHECK(aResult.multiRemovalRange.startLine == 1);
    CHECK(aResult.multiRemovalRange.startCharacter == declAStartCol);
    CHECK(aResult.multiRemovalRange.endLine == 1);
    CHECK(aResult.multiRemovalRange.endCharacter == declBStartCol);

    const auto bResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(multiFx.index, 1, multiBCol);
    CHECK(bResult.inDeclaration);
    CHECK(bResult.declaratorCount == 3);
    CHECK(bResult.multiRemovalRange.startLine == 1);
    CHECK(bResult.multiRemovalRange.startCharacter == declAEndCol);
    CHECK(bResult.multiRemovalRange.endLine == 1);
    CHECK(bResult.multiRemovalRange.endCharacter == declBEndCol);

    const auto fnCol = static_cast<uint32_t>(multiCode.find("f"));
    const auto fnResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(multiFx.index, 0, fnCol);
    CHECK_FALSE(fnResult.inDeclaration);
    CHECK(fnResult.declaratorCount == 0);

    angel_lsp::test::NodeIndexFixture empty(nullptr);
    const auto emptyResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(empty.index, 0, 0);
    CHECK_FALSE(emptyResult.inDeclaration);
}
