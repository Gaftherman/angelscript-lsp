#include <doctest/doctest.h>

#include "features/hover/HoverHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/completion/CompletionHandler.h"
#include "features/signature_help/SignatureHelpHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"
#include <vector>
#include <string>
#include <optional>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct AdversarialTestEnv
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///adversarial_test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        AdversarialTestEnv(const std::string &code)
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

        ~AdversarialTestEnv()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<lsp::Hover> Hover(uint32_t line, uint32_t col)
        {
            HoverRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, col } };
            return GetHover(req);
        }

        std::optional<std::vector<lsp::Location>> Def(uint32_t line, uint32_t col)
        {
            DefinitionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, col } };
            return GetDefinition(req);
        }

        std::optional<std::vector<lsp::Location>> TypeDef(uint32_t line, uint32_t col)
        {
            DefinitionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, col } };
            return GetTypeDefinition(req);
        }

        std::vector<lsp::CompletionItem> Complete(uint32_t line, uint32_t col)
        {
            CompletionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, col } };
            return GetCompletion(req);
        }

        std::optional<lsp::SignatureHelp> SigHelp(uint32_t line, uint32_t col)
        {
            SignatureHelpRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, col } };
            return GetSignatureHelp(req);
        }

        lsp::SemanticTokens Tokens()
        {
            SemanticTokensRequest req{ uri, sourceCode, tree, symbolTable };
            return GetSemanticTokens(req);
        }

        bool HasCompletionItem(const std::vector<lsp::CompletionItem> &items, const std::string &label)
        {
            for (const auto &item : items)
            {
                if (item.label == label) return true;
            }
            return false;
        }
    };
}

// =============================================================================
// 1. HOVER ADVERSARIAL TESTS
// =============================================================================

TEST_CASE("Adversarial Hover - Empty File and Whitespace")
{
    {
        AdversarialTestEnv env("");
        CHECK(!env.Hover(0, 0).has_value());
        CHECK(!env.Hover(10, 10).has_value());
    }
    {
        AdversarialTestEnv env("   \n\t\n   \r\n");
        CHECK(!env.Hover(0, 0).has_value());
        CHECK(!env.Hover(1, 0).has_value());
        CHECK(!env.Hover(2, 2).has_value());
    }
}

TEST_CASE("Adversarial Hover - Extreme Out of Bounds Coordinates")
{
    AdversarialTestEnv env("int x = 42;");
    CHECK(!env.Hover(0, 100).has_value());
    CHECK(!env.Hover(100, 0).has_value());
    CHECK(!env.Hover(UINT32_MAX, UINT32_MAX).has_value());
    CHECK(!env.Hover(1000000, 5).has_value());
}

TEST_CASE("Adversarial Hover - Local and Member Markdown Closures")
{
    // Local variable hover check
    {
        AdversarialTestEnv env("void main() { int localX = 10; localX = 20; }");
        auto hover = env.Hover(0, 34);
        REQUIRE(hover.has_value());
        auto content = std::get<lsp::MarkupContent>(hover->contents);
        CHECK(content.value.size() >= 3);
        CHECK(content.value.substr(content.value.size() - 3) == "```");
    }

    // Member access hover check
    {
        AdversarialTestEnv env("class Entity { int hp; } void main() { Entity e; e.hp = 10; }");
        auto hover = env.Hover(0, 52);
        REQUIRE(hover.has_value());
        auto content = std::get<lsp::MarkupContent>(hover->contents);
        CHECK(content.value.size() >= 3);
        CHECK(content.value.substr(content.value.size() - 3) == "```");
    }
}

TEST_CASE("Adversarial Hover - Syntax Error Recovery States")
{
    // Broken class with unclosed methods
    {
        std::string brokenCode = "class BrokenClass { void Incomplete(int a, \n int validField;\n void main() { validField = 10; }";
        AdversarialTestEnv env(brokenCode);
        CHECK_NOTHROW(env.Hover(1, 6));
    }
    // Severely malformed syntax
    {
        std::string chaos = "void () { { { int = ; @@@ ??? # } } }";
        AdversarialTestEnv env(chaos);
        CHECK_NOTHROW(env.Hover(0, 5));
        CHECK_NOTHROW(env.Hover(0, 15));
        CHECK_NOTHROW(env.Hover(0, 25));
    }
}

TEST_CASE("Adversarial Hover - Deeply Nested Scopes and Shadowing")
{
    std::string code =
        "int x = 1;\n"
        "void main() {\n"
        "    int x = 2;\n"
        "    {\n"
        "        int x = 3;\n"
        "        {\n"
        "            int x = 4;\n"
        "            int y = x;\n" // Line 7, col 20: 'x' here is 4
        "        }\n"
        "    }\n"
        "}\n";

    AdversarialTestEnv env(code);
    auto hover = env.Hover(7, 20);
    REQUIRE(hover.has_value());
    auto content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("(local variable) int x") != std::string::npos);
}

TEST_CASE("Adversarial Hover - Doxygen Doc Comments Edge Cases")
{
    // Block comment without closing tag
    {
        std::string unclosedDoc = "/* Unclosed block comment\nvoid BrokenFunc();";
        AdversarialTestEnv env(unclosedDoc);
        CHECK_NOTHROW(env.Hover(1, 6));
    }

    // Line comments with interleaved blank lines
    {
        std::string docWithBlanks =
            "/// @brief First line of brief\n"
            "///\n"
            "/// Second line of brief.\n"
            "/// @param a Input param.\n"
            "/// @return Result.\n"
            "\n"
            "int ComplexDoc(int a);\n";

        AdversarialTestEnv env(docWithBlanks);
        auto hover = env.Hover(6, 6);
        REQUIRE(hover.has_value());
        auto content = std::get<lsp::MarkupContent>(hover->contents);
        CHECK(content.value.find("First line of brief") != std::string::npos);
        CHECK(content.value.find("`a`: Input param.") != std::string::npos);
    }

    // Tricky Doxygen tags: @tparam, \param, \note, \warning, @see
    {
        std::string backslashTags =
            "/**\n"
            " * \\brief Backslash brief.\n"
            " * \\tparam T Template type.\n"
            " * \\param val Value.\n"
            " * \\note Important note.\n"
            " * \\warning Be careful.\n"
            " * \\see OtherFunc, AnotherFunc\n"
            " */\n"
            "void BackslashFunc(int val);\n";

        AdversarialTestEnv env(backslashTags);
        auto hover = env.Hover(8, 6);
        REQUIRE(hover.has_value());
        auto content = std::get<lsp::MarkupContent>(hover->contents);
        CHECK(content.value.find("Backslash brief.") != std::string::npos);
        CHECK(content.value.find("<T>") != std::string::npos);
        CHECK(content.value.find("`val`: Value.") != std::string::npos);
        CHECK(content.value.find("Important note.") != std::string::npos);
        CHECK(content.value.find("Be careful.") != std::string::npos);
        CHECK(content.value.find("OtherFunc, AnotherFunc") != std::string::npos);
    }
}

// =============================================================================
// 2. DEFINITION ADVERSARIAL TESTS
// =============================================================================

TEST_CASE("Adversarial Definition - Empty, Out of Bounds, Whitespace")
{
    AdversarialTestEnv env("");
    CHECK(!env.Def(0, 0).has_value());
    CHECK(!env.Def(100, 100).has_value());
    CHECK(!env.TypeDef(0, 0).has_value());
    CHECK(!env.TypeDef(100, 100).has_value());
}

TEST_CASE("Adversarial Definition - Variable Shadowing Navigation")
{
    std::string code =
        "int target = 0;\n"         // Line 0
        "void Func() {\n"
        "    int target = 1;\n"     // Line 2
        "    {\n"
        "        int target = 2;\n" // Line 4
        "        int use = target;\n"// Line 5, col 19
        "    }\n"
        "}\n";

    AdversarialTestEnv env(code);
    auto def = env.Def(5, 19);
    REQUIRE(def.has_value());
    REQUIRE(def->size() == 1);
    CHECK((*def)[0].range.start.line == 4);
}

TEST_CASE("Adversarial Definition - Direct Base Class Member Resolution")
{
    std::string code =
        "class Parent {\n"
        "    void parentMethod() {}\n"// Line 1
        "}\n"
        "class Child : Parent {\n"
        "    void test() {\n"
        "        this.parentMethod();\n"// Line 5, col 15: parentMethod
        "    }\n"
        "}\n";

    AdversarialTestEnv env(code);

    auto defMethod = env.Def(5, 15);
    REQUIRE(defMethod.has_value());
    REQUIRE(!defMethod->empty());
    CHECK((*defMethod)[0].range.start.line == 1);
}

TEST_CASE("Adversarial Definition - Go to Type Definition")
{
    std::string code =
        "class Weapon {}\n"         // Line 0
        "enum Element { Fire }\n"   // Line 1
        "void main() {\n"
        "    int primitive = 10;\n"
        "    Weapon@ w = null;\n"   // Line 4, col 13: w
        "    Element e = Element::Fire;\n" // Line 5, col 13: e
        "}\n";

    AdversarialTestEnv env(code);

    auto typeDefPrim = env.TypeDef(3, 9);
    CHECK(!typeDefPrim.has_value());

    auto typeDefWeapon = env.TypeDef(4, 13);
    REQUIRE(typeDefWeapon.has_value());
    REQUIRE(!typeDefWeapon->empty());
    CHECK((*typeDefWeapon)[0].range.start.line == 0);

    auto typeDefEnum = env.TypeDef(5, 13);
    REQUIRE(typeDefEnum.has_value());
    REQUIRE(!typeDefEnum->empty());
    CHECK((*typeDefEnum)[0].range.start.line == 1);
}

// =============================================================================
// 3. COMPLETION ADVERSARIAL TESTS
// =============================================================================

TEST_CASE("Adversarial Completion - Empty File and Out of Bounds")
{
    {
        AdversarialTestEnv env("");
        auto items = env.Complete(0, 0);
        CHECK(!items.empty());
        CHECK(env.HasCompletionItem(items, "class"));
        CHECK(env.HasCompletionItem(items, "void"));
    }
    {
        AdversarialTestEnv env("void main() {}");
        CHECK_NOTHROW(env.Complete(9999, 9999));
    }
}

TEST_CASE("Adversarial Completion - Floating Point Literal vs Member Access")
{
    std::string code = "void main() { float f = 123.; }";
    AdversarialTestEnv env(code);
    auto items = env.Complete(0, 28);
    CHECK(!items.empty());
    CHECK(env.HasCompletionItem(items, "return"));
}

TEST_CASE("Adversarial Completion - Non-existent and Invalid Receivers")
{
    std::string code = "void main() { NonExistentType. }";
    AdversarialTestEnv env(code);
    auto items = env.Complete(0, 30);
    CHECK(items.empty());
}

TEST_CASE("Adversarial Completion - Arrow Operator Member Access")
{
    std::string code =
        "class Node {\n"
        "    Node@ next;\n"
        "    int value;\n"
        "}\n"
        "void main() {\n"
        "    Node n;\n"
        "    n->\n"
        "}\n";

    AdversarialTestEnv env(code);
    auto items = env.Complete(6, 7);
    CHECK(env.HasCompletionItem(items, "next"));
    CHECK(env.HasCompletionItem(items, "value"));
    CHECK(!env.HasCompletionItem(items, "while"));
}

TEST_CASE("Adversarial Completion - Incomplete Code and Syntax Error Recovery")
{
    std::string code =
        "class Entity {\n"
        "    int id;\n"
        "}\n"
        "void main() {\n"
        "    Entity e;\n"
        "    if (e.id > 0) {\n"
        "        for (int i = 0; i < 10; ++i) {\n"
        "            e.\n";

    AdversarialTestEnv env(code);
    auto items = env.Complete(7, 14);
    CHECK(env.HasCompletionItem(items, "id"));
}

// =============================================================================
// 4. SIGNATURE HELP ADVERSARIAL TESTS
// =============================================================================

TEST_CASE("Adversarial SignatureHelp - Overloaded Functions with Varying Arities")
{
    std::string code =
        "void Log() {}\n"
        "void Log(int a) {}\n"
        "void Log(int a, float b) {}\n"
        "void Log(int a, float b, string c) {}\n"
        "void main() {\n"
        "    Log();\n"                         // Line 5, col 8
        "    Log(10);\n"                       // Line 6, col 10
        "    Log(10, 2.5);\n"                  // Line 7, col 14
        "    Log(10, 2.5, \"msg\");\n"         // Line 8, col 20
        "    Log(10, 2.5, \"msg\", 999);\n"    // Line 9, col 27 (excess args)
        "}\n";

    AdversarialTestEnv env(code);

    // Param 0
    auto sig0 = env.SigHelp(5, 8);
    REQUIRE(sig0.has_value());
    REQUIRE(sig0->signatures.size() == 4);
    REQUIRE(sig0->activeParameter.has_value());
    CHECK(sig0->activeParameter.value().value() == 0u);

    // Param 1 (after 1st comma)
    auto sig1 = env.SigHelp(7, 12);
    REQUIRE(sig1.has_value());
    REQUIRE(sig1->activeParameter.has_value());
    CHECK(sig1->activeParameter.value().value() == 1u);
    uint32_t activeSig1 = sig1->activeSignature.has_value() ? static_cast<uint32_t>(sig1->activeSignature.value()) : 0u;
    CHECK(sig1->signatures[activeSig1].parameters.has_value());
    CHECK(sig1->signatures[activeSig1].parameters->size() >= 2);

    // Param 2 (after 2nd comma)
    auto sig2 = env.SigHelp(8, 18);
    REQUIRE(sig2.has_value());
    REQUIRE(sig2->activeParameter.has_value());
    CHECK(sig2->activeParameter.value().value() == 2u);
    uint32_t activeSig2 = sig2->activeSignature.has_value() ? static_cast<uint32_t>(sig2->activeSignature.value()) : 0u;
    CHECK(sig2->signatures[activeSig2].parameters.has_value());
    CHECK(sig2->signatures[activeSig2].parameters->size() >= 3);
}

TEST_CASE("Adversarial SignatureHelp - Complex Expressions, Quotes and Braces in Args")
{
    std::string code =
        "void Draw(string title, int x, int y) {}\n"
        "void main() {\n"
        "    Draw(\"hello, world \\\"quoted, string\\\"\", 100, 200);\n"
        "}\n";

    AdversarialTestEnv env(code);

    auto sigInsideString = env.SigHelp(2, 20);
    REQUIRE(sigInsideString.has_value());
    REQUIRE(sigInsideString->activeParameter.has_value());
    CHECK(sigInsideString->activeParameter.value().value() == 0u);

    auto sigAfterString = env.SigHelp(2, 47);
    REQUIRE(sigAfterString.has_value());
    REQUIRE(sigAfterString->activeParameter.has_value());
    CHECK(sigAfterString->activeParameter.value().value() == 1u);
}

TEST_CASE("Adversarial SignatureHelp - Unclosed Parenthesis / Typing in Progress")
{
    std::string incompleteCode =
        "void Compute(int val, float scale) {}\n"
        "void main() {\n"
        "    Compute(42, \n";

    AdversarialTestEnv env(incompleteCode);
    auto sig = env.SigHelp(2, 16);
    if (sig.has_value())
    {
        REQUIRE(!sig->signatures.empty());
        CHECK(sig->signatures[0].label.find("Compute") != std::string::npos);
        REQUIRE(sig->activeParameter.has_value());
        CHECK(sig->activeParameter.value().value() == 1u);
    }
}

// =============================================================================
// 5. SEMANTIC TOKENS ADVERSARIAL TESTS
// =============================================================================

TEST_CASE("Adversarial SemanticTokens - Invariant Verification")
{
    std::string complexCode =
        "#include \"engine.as\"\n"
        "#pragma once\n"
        "\n"
        "shared class BaseNode {\n"
        "    private int m_id = 0;\n"
        "    int id { get const { return m_id; } }\n"
        "}\n"
        "\n"
        "enum Status {\n"
        "    Ok = 200,\n"
        "    Error = 500\n"
        "}\n"
        "\n"
        "funcdef void ActionCallback(int code, const string &in msg);\n"
        "\n"
        "/* Multi-line\n"
        "   Block Comment */\n"
        "void Process(int count, ActionCallback@ cb) {\n"
        "    for (int i = 0; i < count; ++i) {\n"
        "        if (i % 2 == 0) {\n"
        "            cb(i, \"Status: \" + i);\n"
        "        }\n"
        "    }\n"
        "}\n";

    AdversarialTestEnv env(complexCode);
    auto tokens = env.Tokens();

    REQUIRE(tokens.data.size() % 5 == 0);
    REQUIRE(tokens.data.size() > 0);

    const auto &legend = GetSemanticTokensLegend();
    size_t numTokens = tokens.data.size() / 5;

    for (size_t i = 0; i < numTokens; ++i)
    {
        uint32_t deltaLine = tokens.data[i * 5 + 0];
        uint32_t deltaChar = tokens.data[i * 5 + 1];
        uint32_t length    = tokens.data[i * 5 + 2];
        uint32_t tokenType = tokens.data[i * 5 + 3];

        CHECK(length > 0);
        CHECK(tokenType < legend.tokenTypes.size());

        if (deltaLine == 0 && i > 0)
        {
            CHECK(deltaChar > 0);
        }
    }
}

TEST_CASE("Adversarial SemanticTokens - Severely Broken Syntax")
{
    std::string malformed = "class { void () int === @@@ ??? } #include < \"unclosed string";
    AdversarialTestEnv env(malformed);
    lsp::SemanticTokens tokens;
    CHECK_NOTHROW(tokens = env.Tokens());
    CHECK(tokens.data.size() % 5 == 0);
}

// =============================================================================
// 6. REMEDIATION VERIFICATION TESTS
// =============================================================================

TEST_CASE("Adversarial Remediation - Member Access vs Local Variable Shadowing")
{
    std::string code =
        "class Player {\n"
        "    int health;\n"
        "    void attack() {}\n"
        "}\n"
        "void main() {\n"
        "    int health = 100;\n"
        "    int attack = 5;\n"
        "    Player p;\n"
        "    p.health = 50;\n"
        "    p.attack();\n"
        "}\n";

    AdversarialTestEnv env(code);

    // Hover on 'health' in 'p.health' (line 8, col 6) -> Should resolve to property health on Player, not local variable
    auto hoverHealth = env.Hover(8, 6);
    REQUIRE(hoverHealth.has_value());
    auto healthContent = std::get<lsp::MarkupContent>(hoverHealth->contents).value;
    CHECK(healthContent.find("(property)") != std::string::npos);
    CHECK(healthContent.find("(local variable)") == std::string::npos);

    // Definition on 'health' in 'p.health' (line 8, col 6) -> Should point to Player::health at line 1
    auto defHealth = env.Def(8, 6);
    REQUIRE(defHealth.has_value());
    REQUIRE(!defHealth->empty());
    CHECK((*defHealth)[0].range.start.line == 1);

    // Hover on 'attack' in 'p.attack()' (line 9, col 6) -> Should resolve to method attack on Player
    auto hoverAttack = env.Hover(9, 6);
    REQUIRE(hoverAttack.has_value());
    auto attackContent = std::get<lsp::MarkupContent>(hoverAttack->contents).value;
    CHECK(attackContent.find("void Player::attack()") != std::string::npos);
    CHECK(attackContent.find("(local variable)") == std::string::npos);

    // Definition on 'attack' in 'p.attack()' (line 9, col 6) -> Should point to Player::attack at line 2
    auto defAttack = env.Def(9, 6);
    REQUIRE(defAttack.has_value());
    REQUIRE(!defAttack->empty());
    CHECK((*defAttack)[0].range.start.line == 2);
}

TEST_CASE("Adversarial Remediation - Multi-Level Recursive Inheritance & Interface Traversal")
{
    std::string code =
        "interface ISerializable {\n"
        "    void serialize();\n"
        "}\n"
        "class Entity {\n"
        "    int entityId;\n"
        "    void spawn(int x, int y) {}\n"
        "}\n"
        "class Actor : Entity {\n"
        "    float speed;\n"
        "    void move(float dir) {}\n"
        "}\n"
        "class Hero : Actor, ISerializable {\n"
        "    string heroName;\n"
        "    void castSpell(int id) {}\n"
        "}\n"
        "void main() {\n"
        "    Hero h;\n"
        "    h.entityId = 1;\n"
        "    h.speed = 2.5f;\n"
        "    h.heroName = \"Aragorn\";\n"
        "    h.spawn(10, 20);\n"
        "    h.move(1.0f);\n"
        "    h.castSpell(42);\n"
        "    h.serialize();\n"
        "}\n";

    AdversarialTestEnv env(code);

    // 1. Hover on inherited members across levels
    auto hoverEntityId = env.Hover(17, 6); // h.entityId (from grandparent Entity)
    REQUIRE(hoverEntityId.has_value());
    CHECK(std::get<lsp::MarkupContent>(hoverEntityId->contents).value.find("(property)") != std::string::npos);

    auto hoverSpeed = env.Hover(18, 6); // h.speed (from parent Actor)
    REQUIRE(hoverSpeed.has_value());
    CHECK(std::get<lsp::MarkupContent>(hoverSpeed->contents).value.find("(property)") != std::string::npos);

    auto hoverSpawn = env.Hover(20, 6); // h.spawn (from grandparent Entity)
    REQUIRE(hoverSpawn.has_value());
    CHECK(std::get<lsp::MarkupContent>(hoverSpawn->contents).value.find("spawn") != std::string::npos);

    auto hoverSerialize = env.Hover(23, 6); // h.serialize (from interface ISerializable)
    REQUIRE(hoverSerialize.has_value());
    CHECK(std::get<lsp::MarkupContent>(hoverSerialize->contents).value.find("serialize") != std::string::npos);

    // 2. Definition on inherited members across levels
    auto defEntityId = env.Def(17, 6);
    REQUIRE(defEntityId.has_value());
    REQUIRE(!defEntityId->empty());
    CHECK((*defEntityId)[0].range.start.line == 4); // Entity::entityId

    auto defSpeed = env.Def(18, 6);
    REQUIRE(defSpeed.has_value());
    REQUIRE(!defSpeed->empty());
    CHECK((*defSpeed)[0].range.start.line == 8); // Actor::speed

    auto defSpawn = env.Def(20, 6);
    REQUIRE(defSpawn.has_value());
    REQUIRE(!defSpawn->empty());
    CHECK((*defSpawn)[0].range.start.line == 5); // Entity::spawn

    auto defSerialize = env.Def(23, 6);
    REQUIRE(defSerialize.has_value());
    REQUIRE(!defSerialize->empty());
    CHECK((*defSerialize)[0].range.start.line == 1); // ISerializable::serialize

    // 3. Member completion on 'h.' includes all inherited members
    auto completions = env.Complete(17, 6);
    std::unordered_set<std::string> compLabels;
    for (const auto &item : completions)
    {
        compLabels.insert(item.label);
    }
    CHECK(compLabels.count("heroName") > 0);
    CHECK(compLabels.count("castSpell") > 0);
    CHECK(compLabels.count("speed") > 0);
    CHECK(compLabels.count("move") > 0);
    CHECK(compLabels.count("entityId") > 0);
    CHECK(compLabels.count("spawn") > 0);
    CHECK(compLabels.count("serialize") > 0);

    // 4. Signature help on inherited member calls
    auto sigSpawn = env.SigHelp(20, 15); // h.spawn(10, |20) -> second param
    REQUIRE(sigSpawn.has_value());
    REQUIRE(!sigSpawn->signatures.empty());
    CHECK(sigSpawn->signatures[0].label.find("spawn") != std::string::npos);
    REQUIRE(sigSpawn->activeParameter.has_value());
    CHECK(sigSpawn->activeParameter.value().value() == 1u);
}

TEST_CASE("Adversarial Remediation - SignatureHelp with Comments and Template Brackets")
{
    std::string code =
        "void Configure(string tag, int mode, float factor) {}\n"
        "void main() {\n"
        "    Configure(cast<array<int, 2>>(x), /* comment, with, commas */ 42, // inline, comment\n"
        "              3.14f);\n"
        "}\n";

    AdversarialTestEnv env(code);

    // Parameter 0: inside cast<array<int, 2>>(x)
    auto sig0 = env.SigHelp(2, 20);
    REQUIRE(sig0.has_value());
    REQUIRE(sig0->activeParameter.has_value());
    CHECK(sig0->activeParameter.value().value() == 0u);

    // Parameter 1: after 1st argument comma, inside/after comment
    auto sig1 = env.SigHelp(2, 68);
    REQUIRE(sig1.has_value());
    REQUIRE(sig1->activeParameter.has_value());
    CHECK(sig1->activeParameter.value().value() == 1u);

    // Parameter 2: on line 3, after 2nd comma
    auto sig2 = env.SigHelp(3, 16);
    REQUIRE(sig2.has_value());
    REQUIRE(sig2->activeParameter.has_value());
    CHECK(sig2->activeParameter.value().value() == 2u);
}

