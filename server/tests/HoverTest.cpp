#include <doctest/doctest.h>

#include "features/hover/HoverHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/ListPattern.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
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

        std::optional<lsp::Hover> HoverAt(uint32_t line, uint32_t character)
        {
            HoverRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, character } };
            return GetHover(req);
        }
    };

    /**
     * @brief Two files, so a symbol's declaration and the hover position are not in the same text.
     *
     * The single-file environment above cannot see the defect this exists for: with one file,
     * reading the declaration's line out of the hovered file is right by accident.
     */
    struct TwoFileEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;

        std::string declaringUri = "file:///library.as";
        std::string declaringCode;
        std::string usingUri = "file:///main.as";
        std::string usingCode;
        TSTree *tree = nullptr;

        TwoFileEnvironment(const std::string &library, const std::string &main)
            : declaringCode(library), usingCode(main)
        {
            symbolCollector.CollectSymbols(declaringUri, declaringCode, parser, symbolTable);
            symbolCollector.CollectSymbols(usingUri, usingCode, parser, symbolTable);

            tree = parser.Parse(usingCode);
            auto rootScope = scopeCollector.CollectScopes(usingCode, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(usingUri, std::move(rootScope));
            }
        }

        ~TwoFileEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        /** @brief Hovers in main.as, with library.as reachable through readDocument. */
        std::optional<lsp::Hover> HoverAt(uint32_t line, uint32_t character)
        {
            HoverRequest req{
                usingUri, usingCode, tree, symbolTable, scopeIndex,
                lsp::Position{ line, character },
                [this](const std::string &uri) -> const std::string *
                {
                    if (uri == declaringUri) return &declaringCode;
                    if (uri == usingUri) return &usingCode;
                    return nullptr;
                }
            };
            return GetHover(req);
        }

        /** @brief The same hover with no reader at all, which must show no documentation. */
        std::optional<lsp::Hover> HoverAtWithoutReader(uint32_t line, uint32_t character)
        {
            HoverRequest req{ usingUri, usingCode, tree, symbolTable, scopeIndex,
                              lsp::Position{ line, character } };
            return GetHover(req);
        }
    };
}

// A documentation comment sits above the *declaration*, and startLine counts lines in the file
// that declares the symbol - not in the file being hovered over. Pairing the two showed whatever
// happened to be at that line number locally, so the decoy comment below is what this hover used
// to render: a comment about something else entirely, presented as the symbol's documentation.
TEST_CASE("HoverHandler - A cross-file hover reads the declaring file's comment")
{
    TwoFileEnvironment env(
        "// filler\n"
        "// filler\n"
        "/// Fires the weapon and returns whether it hit.\n"
        "bool Fire(int rounds) { return true; }\n",

        "// filler\n"
        "// filler\n"
        "/// THIS IS THE WRONG COMMENT - it describes Reload, not Fire.\n"
        "void Reload() {}\n"
        "void Use() { Fire(1); }\n");

    auto hover = env.HoverAt(4, 14); // 'Fire' in main.as
    REQUIRE(hover.has_value());

    const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(markup.value.find("Fires the weapon") != std::string::npos);
    CHECK(markup.value.find("WRONG COMMENT") == std::string::npos);
}

TEST_CASE("HoverHandler - A method with no comment inherits the interface's")
{
    // SUGG-04. The contract is written on the interface and repeating it on every implementer is
    // what nobody does, so an implementation with no comment of its own should show the one it is
    // implementing rather than nothing.
    TwoFileEnvironment env(
        "interface IWeapon\n"
        "{\n"
        "    /// Fires the weapon and returns whether it hit.\n"
        "    bool Fire(int rounds);\n"
        "}\n",

        "class Rifle : IWeapon\n"
        "{\n"
        "    bool Fire(int rounds) { return true; }\n"
        "}\n");

    auto hover = env.HoverAt(2, 10); // 'Fire' in Rifle
    REQUIRE(hover.has_value());

    const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(markup.value.find("Fires the weapon") != std::string::npos);
}

TEST_CASE("HoverHandler - A method's own comment wins over the interface's")
{
    TwoFileEnvironment env(
        "interface IWeapon\n"
        "{\n"
        "    /// The interface contract.\n"
        "    bool Fire(int rounds);\n"
        "}\n",

        "class Rifle : IWeapon\n"
        "{\n"
        "    /// Rifles fire one round at a time.\n"
        "    bool Fire(int rounds) { return true; }\n"
        "}\n");

    auto hover = env.HoverAt(3, 10);
    REQUIRE(hover.has_value());

    const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(markup.value.find("one round at a time") != std::string::npos);
    CHECK(markup.value.find("interface contract") == std::string::npos);
}

TEST_CASE("HoverHandler - Without a reader a cross-file hover shows no documentation")
{
    // Silence over a guess: the declaring file may have been indexed and released, and a hover
    // that says less is better than one that says something untrue.
    TwoFileEnvironment env(
        "/// Fires the weapon and returns whether it hit.\n"
        "bool Fire(int rounds) { return true; }\n",

        "/// THIS IS THE WRONG COMMENT.\n"
        "void Use() { Fire(1); }\n");

    auto hover = env.HoverAtWithoutReader(1, 14);
    REQUIRE(hover.has_value());

    const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(markup.value.find("WRONG COMMENT") == std::string::npos);
    CHECK(markup.value.find("bool Fire") != std::string::npos);
}

TEST_CASE("HoverHandler - Primitive Type Hover")
{
    TestEnvironment env("void main() { int x = 42; }");
    auto hover = env.HoverAt(0, 15); // 'int'
    REQUIRE(hover.has_value());
    auto content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("(primitive type) int") != std::string::npos);
}

TEST_CASE("HoverHandler - Local Variable and Parameter Hover")
{
    TestEnvironment env("void Foo(float speed) { int count = 10; count = 20; }");
    
    // Hover on 'speed' at column 15
    auto hoverParam = env.HoverAt(0, 15);
    REQUIRE(hoverParam.has_value());
    auto contentParam = std::get<lsp::MarkupContent>(hoverParam->contents);
    CHECK(contentParam.value.find("(parameter) float speed") != std::string::npos);

    // Hover on 'count' at column 42
    auto hoverVar = env.HoverAt(0, 42);
    REQUIRE(hoverVar.has_value());
    auto contentVar = std::get<lsp::MarkupContent>(hoverVar->contents);
    CHECK(contentVar.value.find("(local variable) int count") != std::string::npos);
}

TEST_CASE("HoverHandler - Global Function Hover with Overloads")
{
    std::string code = 
        "/// Calculates distance.\n"
        "float Dist(float x, float y) { return 0.0f; }\n"
        "float Dist(float x, float y, float z) { return 0.0f; }\n"
        "void main() { Dist(1.0, 2.0); }\n";

    TestEnvironment env(code);
    auto hover = env.HoverAt(3, 15); // 'Dist'
    REQUIRE(hover.has_value());
    auto content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("float Dist(float x, float y)") != std::string::npos);
    CHECK(content.value.find("float Dist(float x, float y, float z)") != std::string::npos);
    CHECK(content.value.find("Calculates distance.") != std::string::npos);
}

TEST_CASE("HoverHandler - Class and Member Hover")
{
    std::string code =
        "class Player : BaseEntity {\n"
        "    int health;\n"
        "    void Attack(int dmg) {}\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.Attack(10);\n"
        "}\n";

    TestEnvironment env(code);

    // Hover on class name 'Player' on line 5
    auto hoverClass = env.HoverAt(5, 5);
    REQUIRE(hoverClass.has_value());
    auto contentClass = std::get<lsp::MarkupContent>(hoverClass->contents);
    CHECK(contentClass.value.find("class Player : BaseEntity") != std::string::npos);

    // Hover on member method 'Attack' on line 6
    auto hoverMethod = env.HoverAt(6, 7);
    REQUIRE(hoverMethod.has_value());
    auto contentMethod = std::get<lsp::MarkupContent>(hoverMethod->contents);
    CHECK(contentMethod.value.find("void Player::Attack(int dmg)") != std::string::npos);
}

TEST_CASE("HoverHandler - Invalid Position Returns Nullopt")
{
    TestEnvironment env("void main() { }");
    auto hover = env.HoverAt(0, 14); // inside whitespace
    CHECK(!hover.has_value());
}

TEST_CASE("HoverHandler - Class Method Declaration and Sibling Method Call")
{
    std::string code =
        "class Entity {\n"
        "    /// Takes damage.\n"
        "    void TakeDamage(int dmg) {}\n"
        "}\n"
        "class Player : Entity {\n"
        "    /// Attacks enemy.\n"
        "    void Attack() {\n"
        "        TakeDamage(5);\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);

    // Hover on 'Attack' declaration at line 6
    auto hoverDecl = env.HoverAt(6, 9);
    REQUIRE(hoverDecl.has_value());
    auto contentDecl = std::get<lsp::MarkupContent>(hoverDecl->contents);
    CHECK(contentDecl.value.find("void Player::Attack()") != std::string::npos);
    CHECK(contentDecl.value.find("Attacks enemy.") != std::string::npos);

    // Hover on unqualified inherited 'TakeDamage' call at line 7
    auto hoverCall = env.HoverAt(7, 10);
    REQUIRE(hoverCall.has_value());
    auto contentCall = std::get<lsp::MarkupContent>(hoverCall->contents);
    CHECK(contentCall.value.find("void Entity::TakeDamage(int dmg)") != std::string::npos);
    CHECK(contentCall.value.find("Takes damage.") != std::string::npos);
}

TEST_CASE("HoverHandler - Namespace Function Hover")
{
    std::string code =
        "namespace Game {\n"
        "    /// Spawns entity at location.\n"
        "    void Spawn(int id) {}\n"
        "    void Init() {\n"
        "        Spawn(1);\n"
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Game::Spawn(2);\n"
        "}\n";

    TestEnvironment env(code);

    // Hover on 'Spawn' inside namespace at line 4
    auto hoverInside = env.HoverAt(4, 9);
    REQUIRE(hoverInside.has_value());
    auto contentInside = std::get<lsp::MarkupContent>(hoverInside->contents);
    CHECK(contentInside.value.find("void Game::Spawn(int id)") != std::string::npos);
    CHECK(contentInside.value.find("Spawns entity at location.") != std::string::npos);

    // Hover on 'Game::Spawn' from outside at line 8
    auto hoverOutside = env.HoverAt(8, 12);
    REQUIRE(hoverOutside.has_value());
    auto contentOutside = std::get<lsp::MarkupContent>(hoverOutside->contents);
    CHECK(contentOutside.value.find("void Game::Spawn(int id)") != std::string::npos);
    CHECK(contentOutside.value.find("Spawns entity at location.") != std::string::npos);
}

TEST_CASE("HoverHandler - Shows parameter reference direction on a method")
{
    std::string code =
        "class Store\n"
        "{\n"
        "    void Put(const string &in key, int64 &inout value, bool &out ok) {}\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Store s;\n"
        "    s.Put('a', 1, true);\n"
        "}\n";

    TestEnvironment env(code);

    auto hover = env.HoverAt(7, 7);
    REQUIRE(hover.has_value());
    auto content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("&in key") != std::string::npos);
    CHECK(content.value.find("&inout value") != std::string::npos);
    CHECK(content.value.find("&out ok") != std::string::npos);
}

TEST_CASE("HoverHandler - Shows access modifiers, const and handles on members")
{
    std::string code =
        "class Node\n"
        "{\n"
        "    private const string m_name;\n"
        "    protected Node@ m_next;\n"
        "    private void Detach() const {}\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    Node n;\n"
        "    n.m_name;\n"
        "    n.m_next;\n"
        "    n.Detach();\n"
        "}\n";

    TestEnvironment env(code);

    auto name = env.HoverAt(9, 7);
    REQUIRE(name.has_value());
    CHECK(std::get<lsp::MarkupContent>(name->contents).value.find("private const string m_name") != std::string::npos);

    auto next = env.HoverAt(10, 7);
    REQUIRE(next.has_value());
    CHECK(std::get<lsp::MarkupContent>(next->contents).value.find("protected Node@ m_next") != std::string::npos);

    auto detach = env.HoverAt(11, 7);
    REQUIRE(detach.has_value());
    CHECK(std::get<lsp::MarkupContent>(detach->contents).value.find("private void Node::Detach() const") != std::string::npos);
}

TEST_CASE("HoverHandler - Shows declaration modifiers on a class")
{
    std::string code =
        "shared abstract class Base {}\n"
        "void main()\n"
        "{\n"
        "    Base@ b = null;\n"
        "}\n";

    TestEnvironment env(code);

    auto hover = env.HoverAt(3, 6);
    REQUIRE(hover.has_value());
    CHECK(std::get<lsp::MarkupContent>(hover->contents).value.find("shared abstract class Base") != std::string::npos);
}

TEST_CASE("HoverHandler - Parameter hover keeps its declared type and direction")
{
    std::string code =
        "class Foo {}\n"
        "void run(const string &in key, Foo@ owner, int &out count)\n"
        "{\n"
        "    key;\n"
        "    owner;\n"
        "    count;\n"
        "}\n";

    TestEnvironment env(code);

    auto key = env.HoverAt(3, 5);
    REQUIRE(key.has_value());
    CHECK(std::get<lsp::MarkupContent>(key->contents).value.find("(parameter) const string &in key") != std::string::npos);

    auto owner = env.HoverAt(4, 5);
    REQUIRE(owner.has_value());
    CHECK(std::get<lsp::MarkupContent>(owner->contents).value.find("(parameter) Foo@ owner") != std::string::npos);

    auto count = env.HoverAt(5, 5);
    REQUIRE(count.has_value());
    CHECK(std::get<lsp::MarkupContent>(count->contents).value.find("(parameter) int &out count") != std::string::npos);
}

TEST_CASE("HoverHandler - The same declaration indexed twice is shown once")
{
    // A predefined stub reachable under two URI spellings used to be collected once per spelling.
    // The hover must collapse the identical copies instead of printing the signature twice.
    std::string code =
        "void Ping(int id) {}\n"
        "void main()\n"
        "{\n"
        "    Ping(1);\n"
        "}\n";

    TestEnvironment env(code);
    env.symbolCollector.CollectSymbols("file:///other-spelling.as", code, env.parser, env.symbolTable);

    auto hover = env.HoverAt(3, 5);
    REQUIRE(hover.has_value());
    const std::string rendered = std::get<lsp::MarkupContent>(hover->contents).value;

    const size_t first = rendered.find("void Ping(int id)");
    REQUIRE(first != std::string::npos);
    CHECK(rendered.find("void Ping(int id)", first + 1) == std::string::npos);
}

TEST_CASE("HoverHandler - Distinct overloads are all shown")
{
    std::string code =
        "void Emit(int id) {}\n"
        "void Emit(const string &in name) {}\n"
        "void main()\n"
        "{\n"
        "    Emit(1);\n"
        "}\n";

    TestEnvironment env(code);

    auto hover = env.HoverAt(4, 5);
    REQUIRE(hover.has_value());
    const std::string rendered = std::get<lsp::MarkupContent>(hover->contents).value;

    CHECK(rendered.find("void Emit(int id)") != std::string::npos);
    CHECK(rendered.find("void Emit(const string &in name)") != std::string::npos);
}

TEST_CASE("HoverHandler - Global Variable vs Local Variable Hover")
{
    std::string code =
        "const int g_var = 100;\n"
        "void main()\n"
        "{\n"
        "    int local_var = 42;\n"
        "    g_var;\n"
        "    local_var;\n"
        "}\n";

    TestEnvironment env(code);

    // Hover at g_var declaration
    auto hoverGlobalDecl = env.HoverAt(0, 12);
    REQUIRE(hoverGlobalDecl.has_value());
    std::string textGlobalDecl = std::get<lsp::MarkupContent>(hoverGlobalDecl->contents).value;
    CHECK(textGlobalDecl.find("(global variable)") != std::string::npos);
    CHECK(textGlobalDecl.find("const int g_var") != std::string::npos);

    // Hover at g_var use inside main
    auto hoverGlobalUse = env.HoverAt(4, 5);
    REQUIRE(hoverGlobalUse.has_value());
    std::string textGlobalUse = std::get<lsp::MarkupContent>(hoverGlobalUse->contents).value;
    CHECK(textGlobalUse.find("(global variable)") != std::string::npos);
    CHECK(textGlobalUse.find("const int g_var") != std::string::npos);

    // Hover at local_var
    auto hoverLocal = env.HoverAt(5, 5);
    REQUIRE(hoverLocal.has_value());
    std::string textLocal = std::get<lsp::MarkupContent>(hoverLocal->contents).value;
    CHECK(textLocal.find("(local variable)") != std::string::npos);
    CHECK(textLocal.find("int local_var") != std::string::npos);
}


// `int[]` and `array<int>` are one type - tests/parity/doc_p09_bracket_array_members.as - and the
// bracket spelling used to reach member resolution as plain `int`, which has no members at all.
// Hovering `length` on it produced nothing where the template spelling produced its signature.
TEST_CASE("HoverHandler - A method on a bracket-declared array resolves")
{
    TestEnvironment env(
        "class array<T> { uint length() const; }\n"
        "void main() { int[] a; uint n = a.length(); }\n");

    auto hover = env.HoverAt(1, 34); // 'length'
    REQUIRE(hover.has_value());

    const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(markup.value.find("length") != std::string::npos);
}

TEST_CASE("HoverHandler - A method on a template-declared array resolves too")
{
    // The template spelling was broken in the same place and for the same reason; it only looked
    // healthy because `length`, `size` and `isEmpty` had a hardcoded shortcut elsewhere. A fourth
    // method has no shortcut, so it is what this asks about.
    TestEnvironment env(
        "class array<T> { void insertLast(const T&in value); }\n"
        "void main() { array<int> a; a.insertLast(1); }\n");

    auto hover = env.HoverAt(1, 31); // 'insertLast'
    REQUIRE(hover.has_value());

    const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(markup.value.find("insertLast") != std::string::npos);
}

TEST_CASE("HoverHandler - Bracket and template arrays hover identically")
{
    TestEnvironment bracketEnv(
        "class array<T> { void insertLast(const T&in value); }\n"
        "void main() { int[] a; a.insertLast(1); }\n");

    TestEnvironment templateEnv(
        "class array<T> { void insertLast(const T&in value); }\n"
        "void main() { array<int> a; a.insertLast(1); }\n");

    auto bracketHover = bracketEnv.HoverAt(1, 26);   // 'insertLast'
    auto templateHover = templateEnv.HoverAt(1, 31); // 'insertLast'

    REQUIRE(bracketHover.has_value());
    REQUIRE(templateHover.has_value());
    CHECK(std::get<lsp::MarkupContent>(bracketHover->contents).value ==
          std::get<lsp::MarkupContent>(templateHover->contents).value);
}

// =====================================================================================
// Hovering a property that the class spells as two methods. Same case as the completion tests:
// `e.Health` compiles, and the symbol table holds `get_Health` and `set_Health` and nothing named
// `Health`, so the lookup found nothing and the hover said nothing about a name the build accepts.
// =====================================================================================

TEST_CASE("HoverHandler - A property backed by accessors is described")
{
    const std::string code =
        "class HostEntityA\n"
        "{\n"
        "    int m_health;\n"
        "    int get_Health() const property { return m_health; }\n"
        "    void set_Health(int v) property { m_health = v; }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    HostEntityA e;\n"
        "    e.Health = 100;\n"
        "}\n";

    TestEnvironment env(code);
    auto hover = env.HoverAt(9, 8);

    REQUIRE(hover.has_value());
    const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
    const std::string &text = markup.value;
    INFO(text);

    // Named as the property it is, with the accessors below it as the implementation.
    CHECK(text.find("(property) int Health") != std::string::npos);
    CHECK(text.find("get_Health") != std::string::npos);
}

// =====================================================================================
// Hovering an `#include`.
//
// Reported as a want: the line says `#include "helper.as"` and gives no hint which of the search
// directories won, or whether it resolved at all. The path is the answer, and it is not guessable
// from the line.
//
// Answered from the text rather than from the tree. The grammar gives the whole directive one
// opaque `preproc_directive` node with no structure inside it, so there is no node under the cursor
// to hover.
// =====================================================================================

namespace
{
    /** @brief Hovers one position in a document, with a stub resolver standing in for the disk. */
    std::optional<lsp::Hover> HoverInclude(const std::string &source,
                                           uint32_t line,
                                           uint32_t character,
                                           std::function<std::string(const std::string &)> resolver)
    {
        AngelScriptParser parser;
        TSTree *tree = parser.Parse(source);
        REQUIRE(tree != nullptr);

        SymbolTable table;
        ScopeIndex scopes;
        const std::string uri = "file:///main.as";

        HoverRequest request{ uri, source, tree, table, scopes, lsp::Position{ line, character } };
        request.resolveInclude = std::move(resolver);

        auto hover = GetHover(request);
        ts_tree_delete(tree);
        return hover;
    }

    /** @brief The markdown of a hover, or "" when there was none. */
    std::string HoverText(const std::optional<lsp::Hover> &hover)
    {
        if (!hover.has_value())
            return "";
        if (const auto *content = std::get_if<lsp::MarkupContent>(&hover->contents))
            return content->value;
        return "";
    }
}

TEST_CASE("Hover - An include shows the file it resolves to")
{
    const std::string source = "#include \"helper.as\"\nvoid main() { }\n";

    const auto hover = HoverInclude(source, 0, 12,
        [](const std::string &raw) { return "E:/work/scripts/" + raw; });

    const std::string text = HoverText(hover);
    INFO(text);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#include \"helper.as\"") != std::string::npos);
    CHECK(text.find("E:/work/scripts/helper.as") != std::string::npos);
}

TEST_CASE("Hover - An include that resolves to nothing says so")
{
    // The half a user actually needs. "No hover" and "resolves to nothing" look identical in the
    // editor, and only one of them is a mistake they can fix.
    const std::string source = "#include \"missing.as\"\nvoid main() { }\n";

    const auto hover = HoverInclude(source, 0, 12,
        [](const std::string &) { return std::string(); });

    const std::string text = HoverText(hover);
    INFO(text);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("Does not resolve") != std::string::npos);
}

TEST_CASE("Hover - The whole directive answers, not only the quoted part")
{
    // A reader pointing at the word `include` is asking the same question as one pointing at the
    // filename, so both positions answer. Past the closing quote is a different question and gets
    // no answer at all.
    const std::string source = "#include \"helper.as\"\nvoid main() { }\n";
    const auto resolver = [](const std::string &raw) { return "/scripts/" + raw; };

    CHECK_FALSE(HoverText(HoverInclude(source, 0, 0, resolver)).empty());   // the '#'
    CHECK_FALSE(HoverText(HoverInclude(source, 0, 4, resolver)).empty());   // inside "include"
    CHECK_FALSE(HoverText(HoverInclude(source, 0, 19, resolver)).empty());  // the closing quote
    CHECK(HoverText(HoverInclude(source, 0, 25, resolver)).empty());        // past the end
}

TEST_CASE("Hover - A line that is not an include is left to the ordinary path")
{
    // The control, and it guards the thing that would actually break: this branch runs before the
    // tree is consulted at all, so a loose match here would swallow every other hover in the file.
    const std::string source = "int gCounter = 0;\nvoid main() { }\n";
    const auto resolver = [](const std::string &raw) { return "/scripts/" + raw; };

    const std::string text = HoverText(HoverInclude(source, 0, 5, resolver));
    INFO(text);
    CHECK(text.find("#include") == std::string::npos);
}

TEST_CASE("Hover - A spaced directive is not an include")
{
    // `# include "helper.as"` is not a directive to the compiler - measured - so it must not be one
    // here either. Answering it would tell the reader the line works while the analyzer, one pass
    // away, is calling it an error.
    const std::string source = "# include \"helper.as\"\nvoid main() { }\n";

    const auto hover = HoverInclude(source, 0, 13,
        [](const std::string &raw) { return "/scripts/" + raw; });

    CHECK(HoverText(hover).find("#include") == std::string::npos);
}

TEST_CASE("Hover - Global property accessors show property and accessor declaration")
{
    const std::string source =
        "class CModule {}\n"
        "CModule@ get_g_Module();\n"
        "void main() {\n"
        "    g_Module;\n"
        "}\n";

    TestEnvironment env(source);
    const auto hover = env.HoverAt(3, 6);
    REQUIRE(hover.has_value());
    const std::string text = std::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(text.find("(property) CModule@ g_Module") != std::string::npos);
    CHECK(text.find("CModule@ get_g_Module()") != std::string::npos);
}

TEST_CASE("Hover - Cross-file class hover shows class declaration and doc comment")
{
    TwoFileEnvironment env(
        "// CVar class\n"
        "class CCVar {\n"
        "    void SetInt(int val);\n"
        "}\n",
        "CCVar@ g_MaxMoney;\n"
        "void main() {\n"
        "    CCVar@ cvar = g_MaxMoney;\n"
        "}\n");

    // Hover on CCVar type in declaration
    const auto hoverType = env.HoverAt(0, 2);
    REQUIRE(hoverType.has_value());
    const std::string textType = std::get<lsp::MarkupContent>(hoverType->contents).value;
    CHECK(textType.find("class CCVar") != std::string::npos);
    CHECK(textType.find("CVar class") != std::string::npos);

    // Hover on CCVar type in local variable
    const auto hoverLocalType = env.HoverAt(2, 6);
    REQUIRE(hoverLocalType.has_value());
    const std::string textLocal = std::get<lsp::MarkupContent>(hoverLocalType->contents).value;
    CHECK(textLocal.find("class CCVar") != std::string::npos);
    CHECK(textLocal.find("CVar class") != std::string::npos);

    // Hover on g_MaxMoney variable
    const auto hoverVar = env.HoverAt(0, 10);
    REQUIRE(hoverVar.has_value());
    const std::string textVar = std::get<lsp::MarkupContent>(hoverVar->contents).value;
    CHECK(textVar.find("CCVar@ g_MaxMoney") != std::string::npos);
}

TEST_CASE("Hover - Trailing comment on constructor is rendered in hover")
{
    TestEnvironment env(
        "class Entity {\n"
        "    Entity(); // asBEHAVE_CONSTRUCT;\n"
        "    void Spawn(); // asBEHAVE_SPAWN;\n"
        "};\n");

    const auto hoverConstruct = env.HoverAt(1, 4);
    REQUIRE(hoverConstruct.has_value());
    const std::string textConstruct = std::get<lsp::MarkupContent>(hoverConstruct->contents).value;
    CHECK(textConstruct.find("asBEHAVE_CONSTRUCT;") != std::string::npos);

    const auto hoverMethod = env.HoverAt(2, 9);
    REQUIRE(hoverMethod.has_value());
    const std::string textMethod = std::get<lsp::MarkupContent>(hoverMethod->contents).value;
    CHECK(textMethod.find("asBEHAVE_SPAWN;") != std::string::npos);
}

TEST_CASE("Hover - Predefined stub with inline list pattern allows hover on subsequent lines")
{
    const std::string stub =
        "class array<T>\n"
        "{\n"
        "    array() {repeat T}; // asBEHAVE_LIST_FACTORY\n"
        "    T& opIndex(uint index);\n"
        "}\n";

    const std::string rewritten = RewriteInlineListPatterns(stub);

    AngelScriptParser parser;
    SymbolCollector symbolCollector{ nullptr };
    LocalScopeCollector scopeCollector{ nullptr };
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;

    const std::string stubUri = "file:///as.predefined";
    TSTree *tree = parser.Parse(rewritten);
    symbolCollector.CollectSymbols(stubUri, rewritten, parser, symbolTable);
    auto rootScope = scopeCollector.CollectScopes(rewritten, parser);
    if (rootScope)
    {
        scopeIndex.SetScopeTree(stubUri, std::move(rootScope));
    }

    // Line 3: "    T& opIndex(uint index);" -> column 7 is on "opIndex"
    HoverRequest req{
        stubUri, rewritten, tree, symbolTable, scopeIndex,
        lsp::Position{ 3, 7 }
    };

    auto hover = GetHover(req);
    REQUIRE(hover.has_value());
    const std::string text = std::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(text.find("opIndex") != std::string::npos);

    ts_tree_delete(tree);
}

TEST_CASE("Hover - Multiple overloads display distinct doc comments")
{
    TestEnvironment env(
        "class Worker {\n"
        "    /// First overload docs.\n"
        "    void DoWork(int a);\n"
        "    /// Second overload docs.\n"
        "    void DoWork(string b);\n"
        "};\n"
        "void main() {\n"
        "    Worker w;\n"
        "    w.DoWork(1);\n"
        "}\n");

    // Line 8, column 7 is on "DoWork"
    const auto hover = env.HoverAt(8, 7);
    REQUIRE(hover.has_value());
    const std::string text = std::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(text.find("First overload docs.") != std::string::npos);
}

TEST_CASE("Hover - Variable with default value displays initializer")
{
    TestEnvironment env(
        "namespace INS2_L85A2 {\n"
        "    string SPR_CAT = \"ins2/arf/\";\n"
        "}\n"
        "void main() {\n"
        "    int speed = 40;\n"
        "}\n");

    // Line 1, column 12 is on SPR_CAT
    const auto hover1 = env.HoverAt(1, 12);
    REQUIRE(hover1.has_value());
    const std::string text1 = std::get<lsp::MarkupContent>(hover1->contents).value;
    CHECK(text1.find("(global variable)") != std::string::npos);
    CHECK(text1.find("string SPR_CAT = \"ins2/arf/\"") != std::string::npos);

    // Line 4, column 9 is on speed
    const auto hover2 = env.HoverAt(4, 9);
    REQUIRE(hover2.has_value());
    const std::string text2 = std::get<lsp::MarkupContent>(hover2->contents).value;
    CHECK(text2.find("(local variable)") != std::string::npos);
    CHECK(text2.find("int speed = 40") != std::string::npos);
}

TEST_CASE("Hover - Member access on unqualified namespaced class resolves correctly")
{
    TestEnvironment env(
        "namespace INS2PROP {\n"
        "    class CIns2Prop {\n"
        "        int health;\n"
        "    };\n"
        "}\n"
        "void main() {\n"
        "    CIns2Prop@ n;\n"
        "    n.health;\n"
        "}\n");

    // Line 7, column 7 is on health
    const auto hover = env.HoverAt(7, 7);
    REQUIRE(hover.has_value());
    const std::string text = std::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(text.find("(property) int health") != std::string::npos);
}

TEST_CASE("Hover - Member access on class member variable of unqualified namespaced class")
{
    TestEnvironment env(
        "namespace INS2PROP {\n"
        "    class CIns2Prop {\n"
        "        int health;\n"
        "    };\n"
        "}\n"
        "class Weapon {\n"
        "    CIns2Prop@ n;\n"
        "    void Attack() {\n"
        "        n.health;\n"
        "    }\n"
        "}\n");

    // Line 8, column 11 is on health
    const auto hover = env.HoverAt(8, 11);
    REQUIRE(hover.has_value());
    const std::string text = std::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(text.find("(property) int health") != std::string::npos);
}

TEST_CASE("Hover - Enum member displays default value and property tag")
{
    TestEnvironment env(
        "namespace INS2_L85A2 {\n"
        "    enum INS2_L85A2_Animations {\n"
        "        IDLE = 0\n"
        "    };\n"
        "}\n"
        "void main() {\n"
        "    INS2_L85A2::IDLE;\n"
        "}\n");

    // Line 6, column 18 is on IDLE
    const auto hover = env.HoverAt(6, 18);
    REQUIRE(hover.has_value());
    const std::string text = std::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(text.find("(property)") != std::string::npos);
    CHECK(text.find("IDLE = 0") != std::string::npos);

    TestEnvironment envBare(
        "enum SimpleEnum {\n"
        "    FIRST = 10\n"
        "};\n"
        "void main() {\n"
        "    FIRST;\n"
        "}\n");

    const auto hoverBare = envBare.HoverAt(4, 5);
    REQUIRE(hoverBare.has_value());
    const std::string textBare = std::get<lsp::MarkupContent>(hoverBare->contents).value;
    CHECK(textBare.find("(property)") != std::string::npos);
    CHECK(textBare.find("FIRST = 10") != std::string::npos);
}

TEST_CASE("HoverHandler - Call overload resolution across inheritance hierarchy")
{
    std::string code =
        "class WeaponBase {\n"
        "    bool Deploy(string v, string p, int draw, string model, int body, float speed) { return true; }\n"
        "}\n"
        "class weapon_ins2l85a2 : WeaponBase {\n"
        "    bool Deploy() { return true; }\n"
        "    void Test() {\n"
        "        Deploy(\"v\", \"p\", 1, \"model\", 0, 1.5f);\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);
    // Line 6, column 9 is 'Deploy'
    auto hover = env.HoverAt(6, 9);
    REQUIRE(hover.has_value());
    auto content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("bool WeaponBase::Deploy(string v, string p, int draw, string model, int body, float speed)") != std::string::npos);
    CHECK(content.value.find("bool weapon_ins2l85a2::Deploy()") != std::string::npos);
    // Best matching overload must appear before the 0-arg overload
    size_t posBest = content.value.find("WeaponBase::Deploy");
    size_t posDerived = content.value.find("weapon_ins2l85a2::Deploy");
    CHECK(posBest < posDerived);
}

TEST_CASE("HoverHandler - Call overload resolution fallback when argument type mismatches")
{
    std::string code =
        "class WeaponBase {\n"
        "    bool Deploy(string v, string p, int draw, string model, int body, float speed) { return true; }\n"
        "}\n"
        "class weapon_ins2l85a2 : WeaponBase {\n"
        "    bool Deploy() { return true; }\n"
        "    void Test() {\n"
        "        Deploy(\"v\", \"p\", 1, \"model\", 0, \"(72.0/32.0)\");\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);
    auto hover = env.HoverAt(6, 9);
    REQUIRE(hover.has_value());
    auto content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("bool WeaponBase::Deploy(string v, string p, int draw, string model, int body, float speed)") != std::string::npos);
    size_t posBest = content.value.find("WeaponBase::Deploy");
    size_t posDerived = content.value.find("weapon_ins2l85a2::Deploy");
    CHECK(posBest < posDerived);
}
