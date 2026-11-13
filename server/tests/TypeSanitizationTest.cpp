#include "analysis/types/TypeSanitization.h"

#include "analysis/index/LocalScopeCollector.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <ostream>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{
TEST_SUITE("TypeSanitization")
{
    TEST_CASE("TrailingReferenceModifierLength handles &in, &out, &inout with whitespace")
    {
        // 1. Single &
        CHECK(TrailingReferenceModifierLength("&") == 1);
        CHECK(TrailingReferenceModifierLength("int&") == 1);
        CHECK(TrailingReferenceModifierLength("int &") == 1);

        // 2. &in (length 3 without space, 4 with space between & and in)
        CHECK(TrailingReferenceModifierLength("&in") == 3);
        CHECK(TrailingReferenceModifierLength("Vector&in") == 3);
        CHECK(TrailingReferenceModifierLength("Vector &in") == 3);
        CHECK(TrailingReferenceModifierLength("Vector& in") == 4);
        CHECK(TrailingReferenceModifierLength("Vector & in") == 4);
        CHECK(TrailingReferenceModifierLength("Vector&   in") == 6);

        // 3. &out (length 4 without space, 5 with space between & and out)
        CHECK(TrailingReferenceModifierLength("&out") == 4);
        CHECK(TrailingReferenceModifierLength("Vector&out") == 4);
        CHECK(TrailingReferenceModifierLength("Vector &out") == 4);
        CHECK(TrailingReferenceModifierLength("Vector& out") == 5);
        CHECK(TrailingReferenceModifierLength("Vector & out") == 5);
        CHECK(TrailingReferenceModifierLength("Vector&   out") == 7);

        // 4. &inout (length 6 without space, 7 with space between & and inout)
        CHECK(TrailingReferenceModifierLength("&inout") == 6);
        CHECK(TrailingReferenceModifierLength("Vector&inout") == 6);
        CHECK(TrailingReferenceModifierLength("Vector &inout") == 6);
        CHECK(TrailingReferenceModifierLength("Vector& inout") == 7);
        CHECK(TrailingReferenceModifierLength("Vector & inout") == 7);
        CHECK(TrailingReferenceModifierLength("Vector&   inout") == 9);

        // 5. Types that end with 'in' or 'out' but have no '&' must return 0
        CHECK(TrailingReferenceModifierLength("origin") == 0);
        CHECK(TrailingReferenceModifierLength("spin") == 0);
        CHECK(TrailingReferenceModifierLength("main") == 0);
        CHECK(TrailingReferenceModifierLength("chain") == 0);
        CHECK(TrailingReferenceModifierLength("about") == 0);
        CHECK(TrailingReferenceModifierLength("layout") == 0);
        CHECK(TrailingReferenceModifierLength("string") == 0);
        CHECK(TrailingReferenceModifierLength("Vector") == 0);
        CHECK(TrailingReferenceModifierLength("") == 0);
    }

    TEST_CASE("Invariant: StripTrailingDecorationsView never truncates base type")
    {
        for (int i = 0; i < 20; ++i)
        {
            const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("CustomType");

            // & in (The off-by-one regression test)
            const std::string withIn = "const " + typeName + "& in";
            CHECK(StripTrailingDecorationsView(withIn) == "const " + typeName);

            // & out
            const std::string withOut = "const " + typeName + "& out";
            CHECK(StripTrailingDecorationsView(withOut) == "const " + typeName);

            // & inout
            const std::string withInOut = "const " + typeName + "& inout";
            CHECK(StripTrailingDecorationsView(withInOut) == "const " + typeName);

            // CleanExpressionType (strips leading const and trailing reference)
            CHECK(CleanExpressionType(withIn) == typeName);
            CHECK(CleanExpressionType(withOut) == typeName);
            CHECK(CleanExpressionType(withInOut) == typeName);
        }
    }

    TEST_CASE("LocalScopeCollector normalizes parameter reference types")
    {
        const std::string paramA = angel_lsp::test::GenerateRandomSymbolName("paramA");
        const std::string paramB = angel_lsp::test::GenerateRandomSymbolName("paramB");
        const std::string paramC = angel_lsp::test::GenerateRandomSymbolName("paramC");
        const std::string paramD = angel_lsp::test::GenerateRandomSymbolName("paramD");

        const std::string code =
            fmt::format("void Func(const string& in {}, const Vector &out {}, int & inout {}, float & {})\n"
                        "{{\n"
                        "}}\n",
                        paramA, paramB, paramC, paramD);

        angel_lsp::parser::AngelScriptParser parser;
        LocalScopeCollector collector(nullptr);
        auto scopes = collector.CollectScopes(code, parser);
        REQUIRE(scopes != nullptr);
        REQUIRE(!scopes->children.empty());

        const Scope* funcScope = scopes->children.front().get();
        REQUIRE(funcScope->definitions.size() == 4);

        CHECK(funcScope->definitions[0].name == paramA);
        CHECK(funcScope->definitions[0].typeName == "const string &in");
        CHECK(funcScope->definitions[0].isReference);

        CHECK(funcScope->definitions[1].name == paramB);
        CHECK(funcScope->definitions[1].typeName == "const Vector &out");
        CHECK(funcScope->definitions[1].isReference);

        CHECK(funcScope->definitions[2].name == paramC);
        CHECK(funcScope->definitions[2].typeName == "int &inout");
        CHECK(funcScope->definitions[2].isReference);

        CHECK(funcScope->definitions[3].name == paramD);
        CHECK(funcScope->definitions[3].typeName == "float &");
        CHECK(funcScope->definitions[3].isReference);
    }
}
} // namespace angel_lsp::analysis
