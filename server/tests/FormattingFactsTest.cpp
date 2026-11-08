#include "analysis/index/FormattingFacts.h"

#include "document/Document.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/SyntaxTree.h"

#include <doctest/doctest.h>
#include <ostream>
#include <string>
#include <string_view>

using namespace angel_lsp;

TEST_CASE("FormattingFacts - on-type suppression follows strings, comments and syntax errors")
{
    parser::AngelScriptParser parser;
    const std::string fn = test::GenerateRandomSymbolName("fn");
    const std::string var = test::GenerateRandomSymbolName("v");
    const std::string code = "void " + fn + "()\n{\n    string " + var + " = \"a;b\";\n    // note; here\n}\n";

    parser::TreePtr tree = parser::MakeTreePtr(parser.Parse(code));
    test::NodeIndexFixture fx(tree.get());

    const uint32_t declCol = 4;
    const uint32_t stringCol = static_cast<uint32_t>(code.find("a;b") - code.find("    string")) + 1;
    CHECK_FALSE(analysis::IsOnTypeFormattingSuppressed(fx.index, 2, declCol));
    CHECK(analysis::IsOnTypeFormattingSuppressed(fx.index, 2, stringCol));
    CHECK(analysis::IsOnTypeFormattingSuppressed(fx.index, 3, 10));

    const std::string broken = "void " + fn + "( {\n    int " + var + " = ;\n";
    parser::TreePtr brokenTree = parser::MakeTreePtr(parser.Parse(broken));
    test::NodeIndexFixture brokenFx(brokenTree.get());
    CHECK(analysis::IsOnTypeFormattingSuppressed(brokenFx.index, 1, 4));

    test::NodeIndexFixture empty(nullptr);
    CHECK_FALSE(analysis::IsOnTypeFormattingSuppressed(empty.index, 0, 0));
}

TEST_CASE("FormattingFacts - stub declarations capture namespace name and body bytes")
{
    parser::AngelScriptParser parser;
    const std::string ns = test::GenerateRandomSymbolName("Ns");
    const std::string fn = test::GenerateRandomSymbolName("fn");
    const std::string cls = test::GenerateRandomSymbolName("C");

    SUBCASE("declarations without a namespace")
    {
        const std::string src = "// header\nvoid " + fn + "();\nclass " + cls + " {}\n// trailer\n";
        auto decls = analysis::ExtractStubDeclarations(src, parser);
        REQUIRE(decls.has_value());
        REQUIRE(decls->size() == 2);
        for (const auto& d : *decls)
        {
            CHECK_FALSE(d.isNamespace);
            CHECK_FALSE(d.hasNameAndBody);
        }
        const std::string_view view(src);
        CHECK(view.substr((*decls)[0].startByte, (*decls)[0].endByte - (*decls)[0].startByte).find(fn) !=
              std::string_view::npos);
        CHECK(view.substr((*decls)[1].startByte, 5) == "class");
    }

    SUBCASE("namespace with name and body")
    {
        const std::string src = "namespace " + ns + "\n{\n    void " + fn + "();\n}\n";
        auto decls = analysis::ExtractStubDeclarations(src, parser);
        REQUIRE(decls.has_value());
        REQUIRE(decls->size() == 1);
        const auto& d = decls->front();
        CHECK(d.isNamespace);
        REQUIRE(d.hasNameAndBody);
        const std::string_view view(src);
        CHECK(view.substr(d.nameStartByte, d.nameEndByte - d.nameStartByte) == ns);
        CHECK(view[d.bodyStartByte] == '{');
        CHECK(view[d.bodyEndByte - 1] == '}');
    }

    SUBCASE("syntax error yields nullopt")
    {
        const std::string src = "namespace " + ns + " { void " + fn + "( }\n";
        CHECK_FALSE(analysis::ExtractStubDeclarations(src, parser).has_value());
    }
}
