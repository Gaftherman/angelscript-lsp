#include <doctest/doctest.h>

#include "features/document_symbol/DocumentSymbolHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct DocumentSymbolTestEnv
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        SymbolTable symbolTable;
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        DocumentSymbolTestEnv(const std::string &code)
            : sourceCode(code)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
        }

        ~DocumentSymbolTestEnv()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<DocumentSymbolResult> GetSymbols()
        {
            DocumentSymbolRequest req{ uri, sourceCode, tree, symbolTable };
            return GetDocumentSymbols(req);
        }

        std::optional<DocumentSymbolResult> GetSymbolsNullTree()
        {
            DocumentSymbolRequest req{ uri, sourceCode, nullptr, symbolTable };
            return GetDocumentSymbols(req);
        }
    };

    void ValidateRangeContainment(const lsp::DocumentSymbol &sym)
    {
        // range.start <= selectionRange.start
        if (sym.range.start.line == sym.selectionRange.start.line)
        {
            CHECK(sym.range.start.character <= sym.selectionRange.start.character);
        }
        else
        {
            CHECK(sym.range.start.line <= sym.selectionRange.start.line);
        }

        // selectionRange.end <= range.end
        if (sym.selectionRange.end.line == sym.range.end.line)
        {
            CHECK(sym.selectionRange.end.character <= sym.range.end.character);
        }
        else
        {
            CHECK(sym.selectionRange.end.line <= sym.range.end.line);
        }

        if (sym.children.has_value())
        {
            for (const auto &child : sym.children.value())
            {
                ValidateRangeContainment(child);
            }
        }
    }
}

TEST_CASE("DocumentSymbolHandler - Global Declarations")
{
    std::string code =
        "int g_GlobalCount = 100;\n"
        "void GlobalFunction(int a, float b) {}\n";

    DocumentSymbolTestEnv env(code);
    auto symbols = env.GetSymbols();

    REQUIRE(symbols.has_value());
    REQUIRE(symbols->size() == 2);

    CHECK((*symbols)[0].name == "g_GlobalCount");
    CHECK((*symbols)[0].kind == lsp::SymbolKind::Variable);
    CHECK((*symbols)[0].detail.has_value());
    CHECK((*symbols)[0].detail.value() == "int");

    CHECK((*symbols)[1].name == "GlobalFunction");
    CHECK((*symbols)[1].kind == lsp::SymbolKind::Function);
    CHECK((*symbols)[1].detail.has_value());
    CHECK((*symbols)[1].detail.value().find("void") != std::string::npos);
}

TEST_CASE("DocumentSymbolHandler - Class Hierarchy with Methods, Fields, and Constructors")
{
    std::string code =
        "class Player\n"
        "{\n"
        "    int health;\n"
        "    float speed;\n"
        "    Player() {}\n"
        "    ~Player() {}\n"
        "    void Move(float dx, float dy) {}\n"
        "}\n";

    DocumentSymbolTestEnv env(code);
    auto symbols = env.GetSymbols();

    REQUIRE(symbols.has_value());
    REQUIRE(symbols->size() == 1);

    const auto &player = (*symbols)[0];
    CHECK(player.name == "Player");
    CHECK(player.kind == lsp::SymbolKind::Class);
    CHECK(player.children.has_value());

    const auto &children = player.children.value();
    REQUIRE(children.size() == 5);

    CHECK(children[0].name == "health");
    CHECK(children[0].kind == lsp::SymbolKind::Field);
    CHECK(children[0].detail.value() == "int");

    CHECK(children[1].name == "speed");
    CHECK(children[1].kind == lsp::SymbolKind::Field);
    CHECK(children[1].detail.value() == "float");

    CHECK(children[2].name == "Player");
    CHECK(children[2].kind == lsp::SymbolKind::Constructor);

    CHECK(children[3].name == "~Player");
    CHECK(children[3].kind == lsp::SymbolKind::Constructor);

    CHECK(children[4].name == "Move");
    CHECK(children[4].kind == lsp::SymbolKind::Method);
}

TEST_CASE("DocumentSymbolHandler - Namespace Nesting")
{
    std::string code =
        "namespace Engine\n"
        "{\n"
        "    namespace Graphics\n"
        "    {\n"
        "        class Renderer\n"
        "        {\n"
        "            void Render() {}\n"
        "        }\n"
        "    }\n"
        "}\n";

    DocumentSymbolTestEnv env(code);
    auto symbols = env.GetSymbols();

    REQUIRE(symbols.has_value());
    REQUIRE(symbols->size() == 1);

    const auto &engine = (*symbols)[0];
    CHECK(engine.name == "Engine");
    CHECK(engine.kind == lsp::SymbolKind::Namespace);
    REQUIRE(engine.children.has_value());

    const auto &graphics = engine.children.value()[0];
    CHECK(graphics.name == "Graphics");
    CHECK(graphics.kind == lsp::SymbolKind::Namespace);
    REQUIRE(graphics.children.has_value());

    const auto &renderer = graphics.children.value()[0];
    CHECK(renderer.name == "Renderer");
    CHECK(renderer.kind == lsp::SymbolKind::Class);
    REQUIRE(renderer.children.has_value());

    const auto &renderMethod = renderer.children.value()[0];
    CHECK(renderMethod.name == "Render");
    CHECK(renderMethod.kind == lsp::SymbolKind::Method);
}

TEST_CASE("DocumentSymbolHandler - Interfaces and Methods")
{
    std::string code =
        "interface IUpdatable\n"
        "{\n"
        "    void Update(float dt);\n"
        "    int GetPriority();\n"
        "}\n";

    DocumentSymbolTestEnv env(code);
    auto symbols = env.GetSymbols();

    REQUIRE(symbols.has_value());
    REQUIRE(symbols->size() == 1);

    const auto &iface = (*symbols)[0];
    CHECK(iface.name == "IUpdatable");
    CHECK(iface.kind == lsp::SymbolKind::Interface);
    REQUIRE(iface.children.has_value());

    const auto &children = iface.children.value();
    REQUIRE(children.size() == 2);

    CHECK(children[0].name == "Update");
    CHECK(children[0].kind == lsp::SymbolKind::Method);

    CHECK(children[1].name == "GetPriority");
    CHECK(children[1].kind == lsp::SymbolKind::Method);
}

TEST_CASE("DocumentSymbolHandler - Enums and Enum Members")
{
    std::string code =
        "enum LogLevel\n"
        "{\n"
        "    Info = 0,\n"
        "    Warning,\n"
        "    Error = 10\n"
        "}\n";

    DocumentSymbolTestEnv env(code);
    auto symbols = env.GetSymbols();

    REQUIRE(symbols.has_value());
    REQUIRE(symbols->size() == 1);

    const auto &enumSym = (*symbols)[0];
    CHECK(enumSym.name == "LogLevel");
    CHECK(enumSym.kind == lsp::SymbolKind::Enum);
    REQUIRE(enumSym.children.has_value());

    const auto &members = enumSym.children.value();
    REQUIRE(members.size() == 3);

    CHECK(members[0].name == "Info");
    CHECK(members[0].kind == lsp::SymbolKind::EnumMember);
    CHECK(members[0].detail.has_value());
    CHECK(members[0].detail.value() == "= 0");

    CHECK(members[1].name == "Warning");
    CHECK(members[1].kind == lsp::SymbolKind::EnumMember);

    CHECK(members[2].name == "Error");
    CHECK(members[2].kind == lsp::SymbolKind::EnumMember);
    CHECK(members[2].detail.has_value());
    CHECK(members[2].detail.value() == "= 10");
}

TEST_CASE("DocumentSymbolHandler - Virtual Properties, Typedefs, and Funcdefs")
{
    std::string code =
        "typedef uint EntityId;\n"
        "funcdef void EventCallback(int id, float value);\n"
        "class Canvas\n"
        "{\n"
        "    int width { get const; set; }\n"
        "}\n";

    DocumentSymbolTestEnv env(code);
    auto symbols = env.GetSymbols();

    REQUIRE(symbols.has_value());
    REQUIRE(symbols->size() == 3);

    CHECK((*symbols)[0].name == "EntityId");
    CHECK((*symbols)[0].kind == lsp::SymbolKind::Class);
    CHECK((*symbols)[0].detail.value().find("typedef") != std::string::npos);

    CHECK((*symbols)[1].name == "EventCallback");
    CHECK((*symbols)[1].kind == lsp::SymbolKind::Function);
    CHECK((*symbols)[1].detail.value().find("funcdef") != std::string::npos);

    const auto &canvas = (*symbols)[2];
    CHECK(canvas.name == "Canvas");
    REQUIRE(canvas.children.has_value());
    CHECK(canvas.children.value()[0].name == "width");
    CHECK(canvas.children.value()[0].kind == lsp::SymbolKind::Property);
}

TEST_CASE("DocumentSymbolHandler - Multiple Variable Declarators in One Statement")
{
    std::string code = "int posX = 1, posY = 2, posZ = 3;\n";

    DocumentSymbolTestEnv env(code);
    auto symbols = env.GetSymbols();

    REQUIRE(symbols.has_value());
    REQUIRE(symbols->size() == 3);

    CHECK((*symbols)[0].name == "posX");
    CHECK((*symbols)[0].kind == lsp::SymbolKind::Variable);
    CHECK((*symbols)[0].detail.value() == "int");

    CHECK((*symbols)[1].name == "posY");
    CHECK((*symbols)[1].kind == lsp::SymbolKind::Variable);
    CHECK((*symbols)[1].detail.value() == "int");

    CHECK((*symbols)[2].name == "posZ");
    CHECK((*symbols)[2].kind == lsp::SymbolKind::Variable);
    CHECK((*symbols)[2].detail.value() == "int");
}

TEST_CASE("DocumentSymbolHandler - Range Invariants and Selection Range Containment")
{
    std::string code =
        "namespace MyNamespace\n"
        "{\n"
        "    class ComplexActor\n"
        "    {\n"
        "        int m_id = 0;\n"
        "        ComplexActor() {}\n"
        "        void Process() {}\n"
        "    }\n"
        "    enum Status { Ok, Failed }\n"
        "}\n";

    DocumentSymbolTestEnv env(code);
    auto symbols = env.GetSymbols();

    REQUIRE(symbols.has_value());
    for (const auto &sym : *symbols)
    {
        ValidateRangeContainment(sym);
    }
}

TEST_CASE("DocumentSymbolHandler - Null Tree Fallback and Empty Document")
{
    std::string code =
        "class TestClass\n"
        "{\n"
        "    void DoWork() {}\n"
        "}\n";

    DocumentSymbolTestEnv env(code);

    auto symbolsFromTree = env.GetSymbols();
    auto symbolsNullTree = env.GetSymbolsNullTree();

    REQUIRE(symbolsFromTree.has_value());
    REQUIRE(symbolsNullTree.has_value());
    CHECK(symbolsFromTree->size() == symbolsNullTree->size());
    CHECK((*symbolsFromTree)[0].name == (*symbolsNullTree)[0].name);

    DocumentSymbolTestEnv emptyEnv("");
    auto emptySymbols = emptyEnv.GetSymbols();
    REQUIRE(emptySymbols.has_value());
    CHECK(emptySymbols->empty());
}
