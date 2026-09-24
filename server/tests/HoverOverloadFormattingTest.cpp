#include <doctest/doctest.h>

#include "features/hover/HoverHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"

#include <string>

using namespace angel_lsp;
using namespace angel_lsp::features;

namespace
{
/**
 * @brief Helper test environment for exercising hover on AngelScript snippets.
 */
struct HoverTestEnvironment
{
    parser::AngelScriptParser parser;
    analysis::SymbolCollector collector{nullptr};
    analysis::LocalScopeCollector scopes{nullptr};
    analysis::SymbolTable table;
    analysis::ScopeIndex scopeIndex;
    std::string uri{"file:///HoverTest.as"};

    /**
     * @brief Queries hover at the specified 0-indexed line and character.
     * @param[in] code Source code string.
     * @param[in] line Line coordinate.
     * @param[in] character Character column.
     * @return Optional LSP Hover markup.
     */
    std::optional<lsp::Hover> HoverAt(const std::string& code, uint32_t line, uint32_t character)
    {
        collector.CollectSymbols(uri, code, parser, table);
        auto rootScope = scopes.CollectScopes(code, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(uri, std::move(rootScope));
        }

        TSTree* tree = parser.Parse(code);
        HoverRequest req{uri, code, tree, table, scopeIndex, lsp::Position{line, character}};
        auto res = GetHover(req);
        if (tree)
        {
            ts_tree_delete(tree);
        }
        return res;
    }
};
} // namespace

TEST_CASE("HoverOverload - Call-site hover prioritizes matched overload and only its doc comment")
{
    HoverTestEnvironment env;
    const std::string className = test::GenerateRandomSymbolName("CScheduler");
    const std::string fnName = test::GenerateRandomSymbolName("SetInterval");

    const std::string code =
        "class " + className + " {\n"
        "    /// Schedules a function string repeatedly.\n"
        "    void " + fnName + "(const string &in szFunc, float flRepeat);\n"
        "    /// Schedules a method call on a target object.\n"
        "    void " + fnName + "(? &in obj, const string &in szFunc, float flRepeat);\n"
        "};\n"
        "void main()\n"
        "{\n"
        "    " + className + " scheduler;\n"
        "    scheduler." + fnName + "(\"tick\", 1.0f);\n"
        "}\n";

    size_t callPos = code.find("scheduler." + fnName);
    REQUIRE(callPos != std::string::npos);
    size_t targetPos = callPos + std::string("scheduler.").length();
    size_t lastNewline = code.rfind('\n', targetPos);
    uint32_t line = 0;
    for (size_t i = 0; i < targetPos; ++i)
    {
        if (code[i] == '\n')
        {
            ++line;
        }
    }
    uint32_t col = static_cast<uint32_t>(targetPos - lastNewline - 1);

    auto hover = env.HoverAt(code, line, col);
    REQUIRE(hover.has_value());
    const auto& content = std::get<lsp::MarkupContent>(hover->contents);

    CHECK(content.value.find("void " + className + "::" + fnName + "(const string &in szFunc, float flRepeat)") != std::string::npos);
    CHECK(content.value.find("Schedules a function string repeatedly.") != std::string::npos);
    CHECK(content.value.find("Schedules a method call on a target object.") == std::string::npos);
}

TEST_CASE("HoverOverload - Anonymous function lambda hover shows target funcdef and its doc comments")
{
    HoverTestEnvironment env;
    const std::string hookDefName = test::GenerateRandomSymbolName("PlayerPostThinkHook");
    const std::string registerFn = test::GenerateRandomSymbolName("RegisterHook");

    const std::string code =
        "/// Called on every player think tick.;\n"
        "funcdef void " + hookDefName + "(int player);\n"
        "void " + registerFn + "(int hookId, " + hookDefName + "@ callback);\n"
        "void MapActivate()\n"
        "{\n"
        "    " + registerFn + "(1, function(int player) {\n"
        "        return;\n"
        "    });\n"
        "}\n";

    size_t funcKwPos = code.find("function(int player)");
    REQUIRE(funcKwPos != std::string::npos);
    size_t lastNewline = code.rfind('\n', funcKwPos);
    uint32_t line = 5;
    uint32_t col = static_cast<uint32_t>(funcKwPos - lastNewline - 1);

    auto hover = env.HoverAt(code, line, col + 2); // Hover inside "function"
    REQUIRE(hover.has_value());
    const auto& content = std::get<lsp::MarkupContent>(hover->contents);

    CHECK(content.value.find("(anonymous function) -> " + hookDefName) != std::string::npos);
    CHECK(content.value.find("funcdef void " + hookDefName + "(int player)") != std::string::npos);
    CHECK(content.value.find("Called on every player think tick.") != std::string::npos);
    CHECK(content.value.find("\n;\n") == std::string::npos);
    CHECK(content.value.find("\n\n;") == std::string::npos);
}

TEST_CASE("HoverOverload - Standalone lambda hover displays parameter signature cleanly")
{
    HoverTestEnvironment env;
    const std::string code =
        "void main()\n"
        "{\n"
        "    auto f = function(int x, float y) {\n"
        "        return x + int(y);\n"
        "    };\n"
        "}\n";

    size_t funcKwPos = code.find("function(int x, float y)");
    REQUIRE(funcKwPos != std::string::npos);
    size_t lastNewline = code.rfind('\n', funcKwPos);
    uint32_t line = 2;
    uint32_t col = static_cast<uint32_t>(funcKwPos - lastNewline - 1);

    auto hover = env.HoverAt(code, line, col + 2);
    REQUIRE(hover.has_value());
    const auto& content = std::get<lsp::MarkupContent>(hover->contents);

    CHECK(content.value.find("(anonymous function) function(int x, float y)") != std::string::npos);
}
