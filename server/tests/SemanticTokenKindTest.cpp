#include <doctest/doctest.h>

#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "parser/AngelScriptParser.h"

#include <optional>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// What kind each identifier is reported as. The delta encoding is covered in
// SemanticTokensTest.cpp; this file is only about whether the *classification* is right,
// which is what decides the colour the editor paints.
// =====================================================================================

namespace
{
    struct DecodedToken
    {
        uint32_t line = 0;
        uint32_t startChar = 0;
        uint32_t length = 0;
        uint32_t type = 0;
        uint32_t modifiers = 0;
    };

    /** @brief Undoes the delta encoding so tokens can be looked up by absolute position. */
    std::vector<DecodedToken> Decode(const lsp::SemanticTokens &tokens)
    {
        std::vector<DecodedToken> decoded;
        uint32_t line = 0;
        uint32_t startChar = 0;

        for (size_t i = 0; i + 5 <= tokens.data.size(); i += 5)
        {
            const uint32_t deltaLine = tokens.data[i];
            const uint32_t deltaStart = tokens.data[i + 1];

            line += deltaLine;
            startChar = (deltaLine == 0) ? startChar + deltaStart : deltaStart;

            decoded.push_back(DecodedToken{line, startChar, tokens.data[i + 2], tokens.data[i + 3], tokens.data[i + 4]});
        }

        return decoded;
    }

    /** @brief Legend name of a token type index, for readable failure messages. */
    std::string TypeName(uint32_t type)
    {
        const auto &legend = GetSemanticTokensLegend();
        return type < legend.tokenTypes.size() ? std::string(legend.tokenTypes[type]) : "<out of range>";
    }

    /** @brief The token starting exactly at the given position, if any. */
    std::optional<DecodedToken> TokenAt(const std::vector<DecodedToken> &tokens, uint32_t line, uint32_t startChar)
    {
        for (const auto &token : tokens)
        {
            if (token.line == line && token.startChar == startChar)
                return token;
        }
        return std::nullopt;
    }

    struct TokenFixture
    {
        AngelScriptParser parser;
        SymbolTable table;
        TSTree *tree = nullptr;
        std::string code;
        std::vector<DecodedToken> tokens;

        explicit TokenFixture(std::string source)
            : code(std::move(source))
        {
            tree = parser.Parse(code);
            REQUIRE(tree != nullptr);

            LocalScopeCollector scopeCollector(nullptr);
            std::shared_ptr<const Scope> scopeRoot = scopeCollector.CollectScopes(code, parser);

            SemanticTokensRequest request{"file:///test.as", code, tree, table, scopeRoot};
            tokens = Decode(GetSemanticTokens(request));
        }

        ~TokenFixture()
        {
            if (tree)
                ts_tree_delete(tree);
        }

        /** @brief Asserts the token starting at the position carries the named legend type. */
        void CheckType(uint32_t line, uint32_t startChar, const std::string &expected) const
        {
            const auto token = TokenAt(tokens, line, startChar);
            REQUIRE_MESSAGE(token.has_value(), "no token at line ", line, " char ", startChar);

            INFO("at line ", line, " char ", startChar, ": got '", TypeName(token->type), "', expected '", expected, "'");
            CHECK(TypeName(token->type) == expected);
        }
    };
}

// -------------------------------------------------------------------------------------
// The reported case: a class field painted as a namespace.
// -------------------------------------------------------------------------------------

TEST_CASE("Semantic tokens - a field read inside a method is a property, not a namespace")
{
    // The grammar wraps every bare identifier expression in a scoped_identifier, so an
    // unqualified reference like "f" is (scoped_identifier (identifier)). A highlights pattern
    // matching that inner identifier as a namespace therefore matches every plain variable
    // reference in the language, and outranks the variable pattern on the same range.
    TokenFixture fixture(
        "class MyClass : AnotherClass\n"
        "{\n"
        "    float f;\n"
        "\n"
        "    void MyMethod()\n"
        "    {\n"
        "        f = 1.0f;\n"
        "    }\n"
        "}\n");

    fixture.CheckType(6, 8, "property");
}

TEST_CASE("Semantic tokens - a class field declaration is a property, not a plain variable")
{
    TokenFixture fixture(
        "class MyClass\n"
        "{\n"
        "    float f;\n"
        "}\n");

    fixture.CheckType(2, 10, "property");
}

// -------------------------------------------------------------------------------------
// The same root cause, seen through everything else it affects.
// -------------------------------------------------------------------------------------

TEST_CASE("Semantic tokens - a local variable read is a variable, not a namespace")
{
    TokenFixture fixture(
        "void Main()\n"
        "{\n"
        "    int count = 0;\n"
        "    count = 1;\n"
        "}\n");

    fixture.CheckType(3, 4, "variable");
}

TEST_CASE("Semantic tokens - a global variable read is a variable, not a namespace")
{
    TokenFixture fixture(
        "int g_ammo = 0;\n"
        "void Main()\n"
        "{\n"
        "    g_ammo = 1;\n"
        "}\n");

    fixture.CheckType(3, 4, "variable");
}

TEST_CASE("Semantic tokens - a parameter read is a parameter, not a namespace")
{
    TokenFixture fixture(
        "void Main(int amount)\n"
        "{\n"
        "    amount = 2;\n"
        "}\n");

    fixture.CheckType(2, 4, "parameter");
}

TEST_CASE("Semantic tokens - an unqualified call is a function, not a namespace")
{
    TokenFixture fixture(
        "void Helper() {}\n"
        "void Main()\n"
        "{\n"
        "    Helper();\n"
        "}\n");

    fixture.CheckType(3, 4, "function");
}

// -------------------------------------------------------------------------------------
// What the namespace pattern is actually for, and must keep doing.
// -------------------------------------------------------------------------------------

TEST_CASE("Semantic tokens - a namespace declaration is still a namespace")
{
    TokenFixture fixture(
        "namespace Weapons\n"
        "{\n"
        "    void Fire() {}\n"
        "}\n");

    fixture.CheckType(0, 10, "namespace");
}

TEST_CASE("Semantic tokens - the qualifier of a qualified call is a namespace and the callee is not")
{
    TokenFixture fixture(
        "void Main()\n"
        "{\n"
        "    Weapons::Fire();\n"
        "}\n");

    fixture.CheckType(2, 4, "namespace");
    fixture.CheckType(2, 13, "function");
}

TEST_CASE("Semantic tokens - a base class in an inheritance list is a type, not a namespace")
{
    TokenFixture fixture(
        "class MyClass : AnotherClass\n"
        "{\n"
        "}\n");

    fixture.CheckType(0, 16, "type");
}

TEST_CASE("Semantic tokens - the namespace of a qualified type is a namespace and the type is a type")
{
    // A qualified type puts the "::" in a (scope ...) node rather than inside the
    // scoped_identifier, so it needs its own pattern - the anchor used for qualified
    // expressions cannot see a "::" that is not a child of the scoped_identifier.
    TokenFixture fixture(
        "void Main()\n"
        "{\n"
        "    Weapons::Rifle r;\n"
        "}\n");

    fixture.CheckType(2, 4, "namespace");
    fixture.CheckType(2, 13, "type");
}

TEST_CASE("Semantic tokens - a qualified type inside a template argument keeps its namespace")
{
    TokenFixture fixture(
        "void Main()\n"
        "{\n"
        "    array<Weapons::Ammo> list;\n"
        "}\n");

    fixture.CheckType(2, 10, "namespace");
}

TEST_CASE("Semantic tokens - a qualified enum member keeps its namespace qualifier")
{
    TokenFixture fixture(
        "enum MyEnum { Value }\n"
        "void Main()\n"
        "{\n"
        "    int v = MyEnum::Value;\n"
        "}\n");

    fixture.CheckType(3, 12, "namespace");
}
