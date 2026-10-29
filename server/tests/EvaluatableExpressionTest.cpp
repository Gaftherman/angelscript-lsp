#include "features/breakpoints/EvaluatableExpressionHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <string>

using namespace angel_lsp;
using namespace angel_lsp::features;

TEST_CASE("EvaluatableExpressionHandler - Granular AST Hover Extraction")
{
    parser::AngelScriptParser parser;
    const std::string varData = test::GenerateRandomSymbolName("data");
    const std::string memberIdx = test::GenerateRandomSymbolName("m_Index");

    const std::string code = "void TestFunc() {\n"
                             "    " +
                             varData + "." + memberIdx +
                             " = 10;\n"
                             "    this.m_prefixedItems.get(data.classname, prefixedItem);\n"
                             "    int val = prefixedItem[1];\n"
                             "    idx++;\n"
                             "    g_Config.GetContext(weapon.GetClassname()).primary_ammo;\n"
                             "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);

    // 1. Line 1: Hover on varData should return just varData
    size_t dataPos = code.find(varData);
    REQUIRE(dataPos != std::string::npos);
    // Find column of varData
    size_t line1Start = code.find("    " + varData);
    uint32_t colData = static_cast<uint32_t>(dataPos - line1Start);
    auto resData = EvaluatableExpressionHandler::Extract({root, code, 1, colData + 1});
    REQUIRE(resData.has_value());
    CHECK(resData->expression == varData);

    // 2. Line 1: Hover on memberIdx should return full member expression (varData.memberIdx)
    size_t idxPos = code.find(memberIdx);
    REQUIRE(idxPos != std::string::npos);
    uint32_t colIdx = static_cast<uint32_t>(idxPos - line1Start);
    auto resIdx = EvaluatableExpressionHandler::Extract({root, code, 1, colIdx + 1});
    REQUIRE(resIdx.has_value());
    CHECK(resIdx->expression == varData + "." + memberIdx);

    // 3. Line 2: Hover on method 'get' of call expression should return nullopt (fall back to LSP hover)
    size_t line2Start = code.find("    this.m_prefixedItems.get");
    size_t getPos = code.find(".get(", line2Start);
    REQUIRE(getPos != std::string::npos);
    uint32_t colGet = static_cast<uint32_t>(getPos + 1 - line2Start);
    auto resGet = EvaluatableExpressionHandler::Extract({root, code, 2, colGet + 1});
    CHECK_FALSE(resGet.has_value());

    // 4. Line 3: Hover on '[' or ']' of index expression returns full index expression
    size_t line3Start = code.find("    int val = prefixedItem[1];");
    size_t bracketPos = code.find("[", line3Start);
    REQUIRE(bracketPos != std::string::npos);
    uint32_t colBracket = static_cast<uint32_t>(bracketPos - line3Start);
    auto resBracket = EvaluatableExpressionHandler::Extract({root, code, 3, colBracket});
    REQUIRE(resBracket.has_value());
    CHECK(resBracket->expression == "prefixedItem[1]");

    // 5. Line 4: Hover on idx in 'idx++' unwraps to 'idx'
    size_t line4Start = code.find("    idx++;");
    size_t idxIncPos = code.find("idx", line4Start);
    REQUIRE(idxIncPos != std::string::npos);
    uint32_t colIdxInc = static_cast<uint32_t>(idxIncPos - line4Start);
    auto resIdxInc = EvaluatableExpressionHandler::Extract({root, code, 4, colIdxInc});
    REQUIRE(resIdxInc.has_value());
    CHECK(resIdxInc->expression == "idx");

    // 6. Line 5: Hover on primary_ammo returns full chained expression including calls
    size_t line5Start = code.find("    g_Config.GetContext");
    size_t ammoPos = code.find("primary_ammo", line5Start);
    REQUIRE(ammoPos != std::string::npos);
    uint32_t colAmmo = static_cast<uint32_t>(ammoPos - line5Start);
    auto resAmmo = EvaluatableExpressionHandler::Extract({root, code, 5, colAmmo});
    REQUIRE(resAmmo.has_value());
    CHECK(resAmmo->expression == "g_Config.GetContext(weapon.GetClassname()).primary_ammo");

    ts_tree_delete(tree);
}
