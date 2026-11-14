#include "analysis/index/CompletionFacts.h"

#include "document/Document.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/SyntaxTree.h"

#include <doctest/doctest.h>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

using namespace angel_lsp;

TEST_SUITE("analysis/index/CompletionFacts")
{
    /**
     * @brief Returns the source text covered by an optional receiver range, or an empty view when absent.
     * @param[in] code Document text.
     * @param[in] range Receiver range found by FindMemberReceiverRange.
     * @return The receiver spelling, or empty when no range was found.
     */
    static std::string_view ReceiverText(std::string_view code, const std::optional<analysis::IndexedByteRange>& range)
    {
        return range ? code.substr(range->startByte, range->endByte - range->startByte) : std::string_view{};
    }

    TEST_CASE("CompletionFacts - member receiver range covers the expression before the dot")
    {
        parser::AngelScriptParser parser;
        const std::string obj = test::GenerateRandomSymbolName("obj");
        const std::string field = test::GenerateRandomSymbolName("f");
        const std::string fn = test::GenerateRandomSymbolName("fn");

        SUBCASE("plain identifier and chained member")
        {
            const std::string code = "void " + fn + "()\n{\n    " + obj + "." + field + ".x;\n}\n";
            parser::TreePtr tree = parser::MakeTreePtr(parser.Parse(code));
            test::NodeIndexFixture fx(tree.get());

            const auto firstDot = static_cast<uint32_t>(code.find(obj + ".") + obj.size());
            CHECK(ReceiverText(code, analysis::FindMemberReceiverRange(fx.index, code, firstDot)) == obj);

            const auto secondDot = static_cast<uint32_t>(code.find(field + ".x") + field.size());
            CHECK(ReceiverText(code, analysis::FindMemberReceiverRange(fx.index, code, secondDot)) ==
                  obj + "." + field);
        }

        SUBCASE("call and index receivers")
        {
            const std::string code =
                "void " + fn + "()\n{\n    " + obj + "()." + field + ";\n    " + obj + "[0]." + field + ";\n}\n";
            parser::TreePtr tree = parser::MakeTreePtr(parser.Parse(code));
            test::NodeIndexFixture fx(tree.get());

            const auto callDot = static_cast<uint32_t>(code.find("().") + 2);
            CHECK(ReceiverText(code, analysis::FindMemberReceiverRange(fx.index, code, callDot)) == obj + "()");

            const auto indexDot = static_cast<uint32_t>(code.find("[0].") + 3);
            CHECK(ReceiverText(code, analysis::FindMemberReceiverRange(fx.index, code, indexDot)) == obj + "[0]");
        }

        SUBCASE("incomplete member access while typing")
        {
            const std::string code = "void " + fn + "()\n{\n    " + obj + ".\n}\n";
            parser::TreePtr tree = parser::MakeTreePtr(parser.Parse(code));
            test::NodeIndexFixture fx(tree.get());

            const auto dot = static_cast<uint32_t>(code.find(obj + ".") + obj.size());
            CHECK(ReceiverText(code, analysis::FindMemberReceiverRange(fx.index, code, dot)) == obj);
        }

        SUBCASE("no tree or dot at offset zero")
        {
            const std::string code = "." + obj;
            test::NodeIndexFixture empty(nullptr);
            CHECK_FALSE(analysis::FindMemberReceiverRange(empty.index, code, 0).has_value());
            CHECK_FALSE(analysis::FindMemberReceiverRange(empty.index, code, 1).has_value());
        }
    }

    TEST_CASE("CompletionFacts - class body outside callable stops at the first function or block")
    {
        parser::AngelScriptParser parser;
        const std::string cls = test::GenerateRandomSymbolName("C");
        const std::string iface = test::GenerateRandomSymbolName("I");
        const std::string method = test::GenerateRandomSymbolName("m");
        const std::string member = test::GenerateRandomSymbolName("v");

        const std::string code = "class " + cls + "\n{\n    int " + member + ";\n    void " + method +
                                 "()\n    {\n        int x = 0;\n    }\n}\ninterface " + iface + "\n{\n    void " +
                                 method + "();\n}\nvoid " + method + "()\n{\n    int y = 1;\n}\n";
        parser::TreePtr tree = parser::MakeTreePtr(parser.Parse(code));
        test::NodeIndexFixture fx(tree.get());

        CHECK(analysis::IsInClassBodyOutsideCallable(fx.index, 2, 2));
        CHECK_FALSE(analysis::IsInClassBodyOutsideCallable(fx.index, 5, 10));
        CHECK(analysis::IsInClassBodyOutsideCallable(fx.index, 9, 0));
        CHECK_FALSE(analysis::IsInClassBodyOutsideCallable(fx.index, 14, 6));

        test::NodeIndexFixture empty(nullptr);
        CHECK_FALSE(analysis::IsInClassBodyOutsideCallable(empty.index, 0, 0));
    }
}
