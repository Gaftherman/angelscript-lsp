#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include <random>

#include "features/signature_help/SignatureHelpHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct SourcePos
    {
        uint32_t line = 0;
        uint32_t character = 0;
    };

    SourcePos FindPos(const std::string& source, const std::string& needle, size_t startAt = 0)
    {
        size_t pos = source.find(needle, startAt);
        if (pos == std::string::npos) return {0, 0};
        uint32_t line = 0;
        size_t lineStart = 0;
        for (size_t i = 0; i < pos; ++i)
        {
            if (source[i] == '\n')
            {
                ++line;
                lineStart = i + 1;
            }
        }
        return {line, static_cast<uint32_t>(pos - lineStart)};
    }

    struct TestEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        TestEnvironment(const std::string &code)
            : sourceCode(code)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        ~TestEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<lsp::SignatureHelp> SigHelpAt(uint32_t line, uint32_t character)
        {
            SignatureHelpRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, character } };
            return GetSignatureHelp(req);
        }
    };
}

TEST_CASE("SignatureHelpHandler - Basic Function Call")
{
    std::string code = 
        "void Test(int a, float b) {}\n"
        "void main() {\n"
        "    Test(10, 2.5);\n"
        "}\n";

    TestEnvironment env(code);

    // Active param 0: inside 'Test(10,' at column 10
    auto sig0 = env.SigHelpAt(2, 10);
    REQUIRE(sig0.has_value());
    REQUIRE(!sig0->signatures.empty());
    CHECK(sig0->signatures[0].label == "void Test(int a, float b)");
    REQUIRE(sig0->activeParameter.has_value());
    CHECK(sig0->activeParameter.value().value() == 0u);

    // Active param 1: inside 'Test(10, 2.5)' at column 14 (after comma)
    auto sig1 = env.SigHelpAt(2, 14);
    REQUIRE(sig1.has_value());
    REQUIRE(sig1->activeParameter.has_value());
    CHECK(sig1->activeParameter.value().value() == 1u);
}

TEST_CASE("SignatureHelpHandler - Nested Comma Handling")
{
    std::string code = 
        "int Inner(int x, int y) { return x + y; }\n"
        "void Outer(int a, int b) {}\n"
        "void main() {\n"
        "    Outer(Inner(1, 2), 3);\n"
        "}\n";

    TestEnvironment env(code);

    // Active param for Outer after inner call and comma (column 24)
    auto sig = env.SigHelpAt(3, 24);
    REQUIRE(sig.has_value());
    CHECK(sig->signatures[0].label.find("void Outer(int a, int b)") != std::string::npos);
    REQUIRE(sig->activeParameter.has_value());
    CHECK(sig->activeParameter.value().value() == 1u);
}

TEST_CASE("SignatureHelpHandler - Member Function Call")
{
    std::string code = 
        "class Player {\n"
        "    void SetSpeed(float s, bool boost = false) {}\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.SetSpeed(5.0);\n"
        "}\n";

    TestEnvironment env(code);

    auto sig = env.SigHelpAt(5, 16);
    REQUIRE(sig.has_value());
    REQUIRE(!sig->signatures.empty());
    CHECK(sig->signatures[0].label == "void Player::SetSpeed(float s, bool boost = false)");
    REQUIRE(sig->activeParameter.has_value());
    CHECK(sig->activeParameter.value().value() == 0u);
}

TEST_CASE("SignatureHelpHandler - Outside Function Call Returns Nullopt")
{
    TestEnvironment env("void main() { int x = 10; }");
    auto sig = env.SigHelpAt(0, 15);
    CHECK(!sig.has_value());
}

// =====================================================================================
// Member calls on an array.
//
// Characterisation first, because the answer here was not what reading the code suggested. This
// file's own CleanBaseType shadows analysis::CleanBaseType and is weaker - it strips `@`, `&` and a
// leading `const` and stops there - so `array<Foo>` and `Foo[]` reached the hierarchy lookup with
// their brackets still on. Neither spelling is a key in the symbol table: a template class is
// registered under its bare name, so `array<Foo>::insertLast` is stored as `array::insertLast`.
// =====================================================================================

TEST_CASE("SignatureHelpHandler - A member call on an array resolves to the template's member")
{
    std::string code =
        "class array<T> { void insertLast(const T&in value); }\n"
        "class Foo {}\n"
        "void main() {\n"
        "    array<Foo> items;\n"
        "    items.insertLast();\n"
        "}\n";

    TestEnvironment env(code);
    auto help = env.SigHelpAt(4, 21);

    REQUIRE(help.has_value());
    REQUIRE_FALSE(help->signatures.empty());
    CHECK(help->signatures[0].label.find("insertLast") != std::string::npos);
}

TEST_CASE("SignatureHelpHandler - The bracket spelling of an array resolves the same way")
{
    // `Foo[]` and `array<Foo>` are the same type written two ways, so a member call on one has to
    // find what a member call on the other finds.
    std::string code =
        "class array<T> { void insertLast(const T&in value); }\n"
        "class Foo {}\n"
        "void main() {\n"
        "    Foo[] items;\n"
        "    items.insertLast();\n"
        "}\n";

    TestEnvironment env(code);
    auto help = env.SigHelpAt(4, 21);

    REQUIRE(help.has_value());
    REQUIRE_FALSE(help->signatures.empty());
    CHECK(help->signatures[0].label.find("insertLast") != std::string::npos);
}

TEST_CASE("SignatureHelpHandler - Invariant: Active parameter index tracks comma positions")
{
    std::mt19937_64 rng(0x1337BEEF);
    const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "MultiParamFunc");
    const std::string code =
        "void " + fnName + "(int a, float b, string c) {}\n"
        "void main() {\n"
        "    " + fnName + "(10, 20.0f, \"text\");\n"
        "}\n";

    TestEnvironment env(code);
    const size_t callLineStart = code.find(fnName + "(10");

    auto posArg0 = FindPos(code, "10", callLineStart);
    auto help0 = env.SigHelpAt(posArg0.line, posArg0.character);
    REQUIRE(help0.has_value());
    REQUIRE(help0->activeParameter.has_value());
    CHECK(help0->activeParameter.value().value() == 0u);

    auto posArg1 = FindPos(code, "20.0f", callLineStart);
    auto help1 = env.SigHelpAt(posArg1.line, posArg1.character);
    REQUIRE(help1.has_value());
    REQUIRE(help1->activeParameter.has_value());
    CHECK(help1->activeParameter.value().value() == 1u);

    auto posArg2 = FindPos(code, "\"text\"", callLineStart);
    auto help2 = env.SigHelpAt(posArg2.line, posArg2.character);
    REQUIRE(help2.has_value());
    REQUIRE(help2->activeParameter.has_value());
    CHECK(help2->activeParameter.value().value() == 2u);
}
