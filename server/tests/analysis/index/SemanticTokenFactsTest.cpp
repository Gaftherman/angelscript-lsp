/**
 * @file SemanticTokenFactsTest.cpp
 * @brief Unit tests for Pass 1 semantic token facts extraction and indexing.
 */

#include "analysis/index/SemanticTokenFacts.h"

#include "analysis/index/NodeIndex.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <ostream>
#include <string>
#include <string_view>

TEST_SUITE("analysis/index/SemanticTokenFacts")
{
    TEST_CASE("SemanticTokenFacts - Template angle brackets flagged as template punctuation")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("CustomType");
        const std::string varName = angel_lsp::test::GenerateRandomSymbolName("myVar");
        const std::string code = "array<array<" + typeName + ">> " + varName +
                                 ";\n"
                                 "int shifted = 1 << 2;\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                                 angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto facts = angel_lsp::analysis::CollectSemanticTokenFacts(nodeIndex, code);

        // Verify template.list captures exist
        size_t templateListCount = 0;
        for (const auto& cap : facts.captures)
        {
            if (cap.tokenType == angel_lsp::analysis::TokenTypeIndex::Type_TemplatePunctuation)
            {
                ++templateListCount;
            }
        }
        CHECK(templateListCount == 2);

        ts_tree_delete(tree);
    }

    TEST_CASE("SemanticTokenFacts - Base-list scoped leaf has isBaseListScopedLeaf and qualifier range")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("WeaponNS");
        const std::string baseName = angel_lsp::test::GenerateRandomSymbolName("BaseWeapon");
        const std::string derivedName = angel_lsp::test::GenerateRandomSymbolName("DerivedWeapon");
        const std::string code = "namespace " + nsName +
                                 " {\n"
                                 "    class " +
                                 baseName +
                                 " {}\n"
                                 "}\n"
                                 "class " +
                                 derivedName + " : " + nsName + "::" + baseName + " {}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                                 angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto facts = angel_lsp::analysis::CollectSemanticTokenFacts(nodeIndex, code);

        bool foundBaseListLeaf = false;
        for (const auto& cap : facts.captures)
        {
            if (cap.isBaseListScopedLeaf)
            {
                foundBaseListLeaf = true;
                std::string_view leafText(code.data() + cap.startByte, cap.endByte - cap.startByte);
                CHECK(leafText == baseName);
                REQUIRE(cap.qualifierEndByte > cap.qualifierStartByte);
                std::string_view qualText(code.data() + cap.qualifierStartByte,
                                          cap.qualifierEndByte - cap.qualifierStartByte);
                CHECK(qualText == nsName);
            }
        }
        CHECK(foundBaseListLeaf);

        ts_tree_delete(tree);
    }

    TEST_CASE("SemanticTokenFacts - Scoped identifier and scoped type ranges spell expected text")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("MathNS");
        const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Vector3");
        const std::string memberName = angel_lsp::test::GenerateRandomSymbolName("Zero");
        const std::string code = "namespace " + nsName +
                                 " {\n"
                                 "    class " +
                                 typeName +
                                 " {}\n"
                                 "}\n"
                                 "void Run() {\n"
                                 "    " +
                                 nsName + "::" + typeName +
                                 " v;\n"
                                 "    auto x = " +
                                 nsName + "::" + memberName +
                                 ";\n"
                                 "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                                 angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto facts = angel_lsp::analysis::CollectSemanticTokenFacts(nodeIndex, code);

        bool foundScopedType = false;
        for (const auto& st : facts.scopedTypes)
        {
            std::string_view qualText(code.data() + st.scopeTextStartByte, st.scopeTextEndByte - st.scopeTextStartByte);
            std::string_view nameText(code.data() + st.memberTextStartByte,
                                      st.memberTextEndByte - st.memberTextStartByte);
            if (qualText == nsName && nameText == typeName)
            {
                foundScopedType = true;
            }
        }
        CHECK(foundScopedType);

        bool foundScopedIdent = false;
        for (const auto& si : facts.scopedNames)
        {
            std::string_view qualText(code.data() + si.leftStartByte, si.leftEndByte - si.leftStartByte);
            std::string_view nameText(code.data() + si.rightStartByte, si.rightEndByte - si.rightStartByte);
            if (qualText == nsName && nameText == memberName)
            {
                foundScopedIdent = true;
            }
        }
        CHECK(foundScopedIdent);

        ts_tree_delete(tree);
    }

    TEST_CASE("SemanticTokenFacts - Reference modifier & is classified as Type_Modifier")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Player");
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("UpdatePlayer");
        const std::string code = "class " + typeName +
                                 " {}\n"
                                 "void " +
                                 fnName + "(" + typeName + "&out p) {}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                                 angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto facts = angel_lsp::analysis::CollectSemanticTokenFacts(nodeIndex, code);

        bool foundAmpModifier = false;
        for (const auto& cap : facts.captures)
        {
            if (cap.tokenType == angel_lsp::analysis::TokenTypeIndex::Type_Modifier)
            {
                std::string_view text(code.data() + cap.startByte, cap.endByte - cap.startByte);
                if (text == "&")
                {
                    foundAmpModifier = true;
                }
            }
        }
        CHECK(foundAmpModifier);

        ts_tree_delete(tree);
    }

    TEST_CASE("SemanticTokenFacts - Namespace-enclosed class spans FindEnclosingClass returns qualified inner name")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("OuterNS");
        const std::string innerName = angel_lsp::test::GenerateRandomSymbolName("InnerClass");
        const std::string fieldName = angel_lsp::test::GenerateRandomSymbolName("nestedField");
        const std::string code = "namespace " + nsName +
                                 " {\n"
                                 "    class " +
                                 innerName +
                                 " {\n"
                                 "        int " +
                                 fieldName +
                                 ";\n"
                                 "    }\n"
                                 "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex nodeIndex(ts_tree_root_node(tree),
                                                 angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto facts = angel_lsp::analysis::CollectSemanticTokenFacts(nodeIndex, code);

        const size_t fieldPos = code.find(fieldName);
        REQUIRE(fieldPos != std::string::npos);

        const std::string_view enclosing = facts.FindEnclosingClass(static_cast<uint32_t>(fieldPos));
        const std::string expectedQualified = nsName + "::" + innerName;
        CHECK(enclosing == expectedQualified);

        ts_tree_delete(tree);
    }
}
