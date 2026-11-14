#include "analysis/index/ClassRuleFacts.h"

#include "analysis/index/MixinSourceIndex.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE("analysis/index/ClassRuleFacts")
{
    TEST_CASE("ClassRuleFacts - BuildBaseClassSpans")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string base1 = angel_lsp::test::GenerateRandomSymbolName("Base");
        const std::string derived1 = angel_lsp::test::GenerateRandomSymbolName("Derived");
        const std::string baseA = angel_lsp::test::GenerateRandomSymbolName("BaseA");
        const std::string baseB = angel_lsp::test::GenerateRandomSymbolName("BaseB");
        const std::string baseC = angel_lsp::test::GenerateRandomSymbolName("BaseC");
        const std::string derived3 = angel_lsp::test::GenerateRandomSymbolName("Derived3");
        const std::string simpleClass = angel_lsp::test::GenerateRandomSymbolName("Simple");

        const std::string code = "class " + derived1 + " : " + base1 + " {}\n" + "class " + derived3 + " : " + baseA +
                                 ", " + baseB + ", " + baseC + " {}\n" + "class " + simpleClass + " {}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto spans1 = angel_lsp::analysis::BuildBaseClassSpans(index, 0, 0);
        REQUIRE(spans1.size() == 1);
        CHECK(code.substr(spans1[0].bytes.startByte, spans1[0].bytes.endByte - spans1[0].bytes.startByte) == base1);

        const auto spans3 = angel_lsp::analysis::BuildBaseClassSpans(index, 1, 1);
        REQUIRE(spans3.size() == 3);
        CHECK(code.substr(spans3[0].bytes.startByte, spans3[0].bytes.endByte - spans3[0].bytes.startByte) == baseA);
        CHECK(code.substr(spans3[1].bytes.startByte, spans3[1].bytes.endByte - spans3[1].bytes.startByte) == baseB);
        CHECK(code.substr(spans3[2].bytes.startByte, spans3[2].bytes.endByte - spans3[2].bytes.startByte) == baseC);

        const auto spansNone = angel_lsp::analysis::BuildBaseClassSpans(index, 2, 2);
        CHECK(spansNone.empty());

        const auto spansExcluded = angel_lsp::analysis::BuildBaseClassSpans(index, 10, 20);
        CHECK(spansExcluded.empty());

        ts_tree_delete(tree);
    }

    TEST_CASE("ClassRuleFacts - FindMixinInclusionSpan")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("Mixin");
        const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("Host");
        const std::string absentName = angel_lsp::test::GenerateRandomSymbolName("Absent");

        const std::string code =
            "mixin class " + mixinName + " {}\n" + "class " + hostName + " : " + mixinName + " {}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const angel_lsp::analysis::MixinInclusionQuery query{
            .hostPos = {1, 0},
            .mixinName = mixinName,
            .mixinShortName = mixinName,
        };
        const auto span = angel_lsp::analysis::FindMixinInclusionSpan(index, code, query);
        REQUIRE(span.has_value());
        const uint32_t len = span->bytes.endByte - span->bytes.startByte;
        CHECK(code.substr(span->bytes.startByte, len) == mixinName);

        const angel_lsp::analysis::MixinInclusionQuery absentQuery{
            .hostPos = {1, 0},
            .mixinName = absentName,
            .mixinShortName = absentName,
        };
        const auto absentSpan = angel_lsp::analysis::FindMixinInclusionSpan(index, code, absentQuery);
        CHECK_FALSE(absentSpan.has_value());

        ts_tree_delete(tree);
    }

    TEST_CASE("ClassRuleFacts - BuildMixinBodyFacts")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("Mixin");
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("p");
        const std::string locName = angel_lsp::test::GenerateRandomSymbolName("loc");
        const std::string memberName = angel_lsp::test::GenerateRandomSymbolName("a");
        const std::string callName = angel_lsp::test::GenerateRandomSymbolName("g");
        const std::string identName = angel_lsp::test::GenerateRandomSymbolName("h");

        const std::string code = "mixin class " + mixinName + " {\n" + "    void " + fnName + "(int " + paramName +
                                 ") {\n" + "        int " + locName + " = 1;\n" + "        this." + memberName + " = " +
                                 locName + ";\n" + "        " + callName + "();\n" + "        " + identName + ";\n" +
                                 "    }\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const angel_lsp::analysis::IndexedSourcePoint mixinPos{0, 0};
        const angel_lsp::analysis::MixinBodyFacts facts =
            angel_lsp::analysis::BuildMixinBodyFacts(index, code, mixinPos);

        CHECK(facts.found);
        REQUIRE(facts.functions.size() == 1);

        const auto& fn = facts.functions.front();
        std::vector<std::string> localTexts;
        for (const auto& br : fn.localNames)
        {
            localTexts.push_back(code.substr(br.startByte, br.endByte - br.startByte));
        }
        CHECK(std::find(localTexts.begin(), localTexts.end(), paramName) != localTexts.end());
        CHECK(std::find(localTexts.begin(), localTexts.end(), locName) != localTexts.end());

        REQUIRE(fn.refs.size() >= 3);
        CHECK(fn.refs[0].kind == angel_lsp::analysis::MixinRefKind::ThisMember);
        CHECK(code.substr(fn.refs[0].span.bytes.startByte,
                          fn.refs[0].span.bytes.endByte - fn.refs[0].span.bytes.startByte) == memberName);

        CHECK(fn.refs[1].kind == angel_lsp::analysis::MixinRefKind::Call);
        CHECK(code.substr(fn.refs[1].span.bytes.startByte,
                          fn.refs[1].span.bytes.endByte - fn.refs[1].span.bytes.startByte) == callName);

        bool foundIdentH = false;
        for (size_t i = 2; i < fn.refs.size(); ++i)
        {
            CHECK(fn.refs[i].kind == angel_lsp::analysis::MixinRefKind::Identifier);
            if (code.substr(fn.refs[i].span.bytes.startByte,
                            fn.refs[i].span.bytes.endByte - fn.refs[i].span.bytes.startByte) == identName)
            {
                foundIdentH = true;
            }
        }
        CHECK(foundIdentH);

        const auto itLoc = std::find_if(fn.localNames.begin(), fn.localNames.end(),
                                        [&](const angel_lsp::analysis::IndexedByteRange& br)
                                        { return code.substr(br.startByte, br.endByte - br.startByte) == locName; });
        REQUIRE(itLoc != fn.localNames.end());
        for (const auto& ref : fn.refs)
        {
            if (ref.kind == angel_lsp::analysis::MixinRefKind::Identifier)
            {
                const bool coversDeclarator =
                    ref.span.bytes.startByte == itLoc->startByte && ref.span.bytes.endByte == itLoc->endByte;
                CHECK_FALSE(coversDeclarator);
            }
        }

        ts_tree_delete(tree);
    }

    TEST_CASE("ClassRuleFacts - BuildMixinBodyFacts at a position with no class")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string code = "void " + fnName + "() {}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const angel_lsp::analysis::IndexedSourcePoint pt{0, 0};
        const angel_lsp::analysis::MixinBodyFacts facts = angel_lsp::analysis::BuildMixinBodyFacts(index, code, pt);
        CHECK_FALSE(facts.found);
        CHECK(facts.functions.empty());

        ts_tree_delete(tree);
    }

    TEST_CASE("ClassRuleFacts - BuildMixinFileCacheEntry")
    {
        const std::string className = angel_lsp::test::GenerateRandomSymbolName("Class");
        const std::string code = "class " + className + " {}\n";

        angel_lsp::analysis::MixinFileCacheEntry entry = angel_lsp::analysis::BuildMixinFileCacheEntry(code);
        CHECK(entry.tree != nullptr);
        CHECK(entry.nodeIndex != nullptr);
        CHECK(entry.source == code);

        angel_lsp::analysis::MixinFileCacheEntry emptyEntry = angel_lsp::analysis::BuildMixinFileCacheEntry("");
        CHECK(emptyEntry.nodeIndex == nullptr);
        CHECK(emptyEntry.tree == nullptr);
        CHECK(emptyEntry.source.empty());
    }
}
