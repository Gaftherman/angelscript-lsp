#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "features/hover/HoverHandler.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

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
     * @param[in] i18n Optional localization pointer.
     * @return Optional LSP Hover markup.
     */
    std::optional<lsp::Hover> HoverAt(const std::string& code, uint32_t line, uint32_t character,
                                      const i18n::I18n* i18n = nullptr)
    {
        collector.CollectSymbols(uri, code, parser, table);
        auto rootScope = scopes.CollectScopes(code, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(uri, std::move(rootScope));
        }

        TSTree* tree = parser.Parse(code);
        HoverRequest req{uri, code,    tree, table,   scopeIndex, lsp::Position{line, character},
                         {},  nullptr, {},   nullptr, i18n};
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

    const std::string code = "class " + className +
                             " {\n"
                             "    /// Schedules a function string repeatedly.\n"
                             "    void " +
                             fnName +
                             "(const string &in szFunc, float flRepeat);\n"
                             "    /// Schedules a method call on a target object.\n"
                             "    void " +
                             fnName +
                             "(? &in obj, const string &in szFunc, float flRepeat);\n"
                             "};\n"
                             "void main()\n"
                             "{\n"
                             "    " +
                             className +
                             " scheduler;\n"
                             "    scheduler." +
                             fnName +
                             "(\"tick\", 1.0f);\n"
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

    CHECK(content.value.find("void " + className + "::" + fnName + "(const string &in szFunc, float flRepeat)") !=
          std::string::npos);
    CHECK(content.value.find("1 more overload") != std::string::npos);
    CHECK(content.value.find("(? &in obj") == std::string::npos);
    CHECK(content.value.find("Schedules a function string repeatedly.") != std::string::npos);
    CHECK(content.value.find("Schedules a method call on a target object.") == std::string::npos);
}

TEST_CASE("HoverOverload - Anonymous function lambda hover shows target funcdef and its doc comments")
{
    HoverTestEnvironment env;
    const std::string hookDefName = test::GenerateRandomSymbolName("PlayerPostThinkHook");
    const std::string registerFn = test::GenerateRandomSymbolName("RegisterHook");

    const std::string code = "/// Called on every player think tick.;\n"
                             "funcdef void " +
                             hookDefName +
                             "(int player);\n"
                             "void " +
                             registerFn + "(int hookId, " + hookDefName +
                             "@ callback);\n"
                             "void MapActivate()\n"
                             "{\n"
                             "    " +
                             registerFn +
                             "(1, function(int player) {\n"
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
    const std::string code = "void main()\n"
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

TEST_CASE("HoverOverload - Method call with multiple overloads shows single resolved signature and localized summary "
          "in Spanish and English")
{
    const std::string className = test::GenerateRandomSymbolName("CJson");
    const std::string fnName = test::GenerateRandomSymbolName("Get");

    // 9 overloads of Get, replicating user's scenario
    const std::string code = "class " + className +
                             " {\n"
                             "    /// Gets float value.\n"
                             "    bool " +
                             fnName +
                             "(const string &in key, float &out val, bool strict = true) const;\n"
                             "    bool " +
                             fnName +
                             "(const string &in key, int &out val, bool strict = true) const;\n"
                             "    bool " +
                             fnName +
                             "(const string &in key, bool &out val, bool strict = true) const;\n"
                             "    bool " +
                             fnName +
                             "(const string &in key, string &out val, bool strict = true) const;\n"
                             "    bool " +
                             fnName +
                             "(bool &out val, bool strict = true) const;\n"
                             "    bool " +
                             fnName +
                             "(int &out val, bool strict = true) const;\n"
                             "    bool " +
                             fnName +
                             "(float &out val, bool strict = true) const;\n"
                             "    bool " +
                             fnName +
                             "(string &out val, bool strict = true) const;\n"
                             "    bool " +
                             fnName + "(const string &in key, " + className +
                             "@ &out val) const;\n"
                             "};\n"
                             "void main() {\n"
                             "    " +
                             className +
                             " obj;\n"
                             "    float outVal = 0.0f;\n"
                             "    obj." +
                             fnName +
                             "(\"testKey\", outVal);\n"
                             "}\n";

    size_t callPos = code.find("obj." + fnName);
    REQUIRE(callPos != std::string::npos);
    size_t targetPos = callPos + std::string("obj.").length();
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

    // Test with Spanish locale
    {
        HoverTestEnvironment env;
        i18n::I18n i18nEs("es");
        auto hoverEs = env.HoverAt(code, line, col, &i18nEs);
        REQUIRE(hoverEs.has_value());
        const auto& content = std::get<lsp::MarkupContent>(hoverEs->contents);

        // Resolved signature must be present
        CHECK(content.value.find("bool " + className + "::" + fnName +
                                 "(const string &in key, float &out val, bool strict = true) const") !=
              std::string::npos);
        // Spanish summary for remaining 8 overloads
        CHECK(content.value.find("8 sobrecargas más") != std::string::npos);
        // Unmatched signatures must NOT be in the code block
        CHECK(content.value.find("int &out val") == std::string::npos);
        CHECK(content.value.find("Gets float value.") != std::string::npos);
    }

    // Test with English locale
    {
        HoverTestEnvironment env;
        i18n::I18n i18nEn("en");
        auto hoverEn = env.HoverAt(code, line, col, &i18nEn);
        REQUIRE(hoverEn.has_value());
        const auto& content = std::get<lsp::MarkupContent>(hoverEn->contents);

        CHECK(content.value.find("bool " + className + "::" + fnName +
                                 "(const string &in key, float &out val, bool strict = true) const") !=
              std::string::npos);
        CHECK(content.value.find("8 more overloads") != std::string::npos);
        CHECK(content.value.find("int &out val") == std::string::npos);
        CHECK(content.value.find("Gets float value.") != std::string::npos);
    }
}

TEST_CASE("HoverOverload - Hovering on declaration selects specific overload and summarizes remaining overloads")
{
    HoverTestEnvironment env;
    const std::string fnName = test::GenerateRandomSymbolName("ExecuteTask");

    const std::string code = "/// First overload.\n"
                             "void " +
                             fnName +
                             "(int taskId);\n"
                             "/// Second overload.\n"
                             "void " +
                             fnName + "(float taskWeight);\n";

    // Hover on line 1 inside first overload declaration
    size_t pos1 = code.find("void " + fnName + "(int");
    REQUIRE(pos1 != std::string::npos);
    size_t targetPos1 = pos1 + std::string("void ").length();
    size_t lastNewline1 = code.rfind('\n', targetPos1);
    uint32_t line1 = 1;
    uint32_t col1 = static_cast<uint32_t>(targetPos1 - lastNewline1 - 1);

    auto hover1 = env.HoverAt(code, line1, col1);
    REQUIRE(hover1.has_value());
    const auto& content1 = std::get<lsp::MarkupContent>(hover1->contents);
    CHECK(content1.value.find("void " + fnName + "(int taskId)") != std::string::npos);
    CHECK(content1.value.find("1 more overload") != std::string::npos);
    CHECK(content1.value.find("taskWeight") == std::string::npos);
    CHECK(content1.value.find("First overload.") != std::string::npos);
    CHECK(content1.value.find("Second overload.") == std::string::npos);

    // Hover on line 3 inside second overload declaration
    size_t pos2 = code.find("void " + fnName + "(float");
    REQUIRE(pos2 != std::string::npos);
    size_t targetPos2 = pos2 + std::string("void ").length();
    size_t lastNewline2 = code.rfind('\n', targetPos2);
    uint32_t line2 = 3;
    uint32_t col2 = static_cast<uint32_t>(targetPos2 - lastNewline2 - 1);

    auto hover2 = env.HoverAt(code, line2, col2);
    REQUIRE(hover2.has_value());
    const auto& content2 = std::get<lsp::MarkupContent>(hover2->contents);
    CHECK(content2.value.find("void " + fnName + "(float taskWeight)") != std::string::npos);
    CHECK(content2.value.find("1 more overload") != std::string::npos);
    CHECK(content2.value.find("taskId") == std::string::npos);
    CHECK(content2.value.find("Second overload.") != std::string::npos);
    CHECK(content2.value.find("First overload.") == std::string::npos);
}

TEST_CASE("HoverOverload - Non-overloaded function shows no overload summary")
{
    HoverTestEnvironment env;
    const std::string fnName = test::GenerateRandomSymbolName("StandaloneFn");

    const std::string code = "void " + fnName +
                             "(int val) {}\n"
                             "void main() {\n"
                             "    " +
                             fnName +
                             "(42);\n"
                             "}\n";

    size_t callPos = code.find(fnName + "(42)");
    REQUIRE(callPos != std::string::npos);
    size_t lastNewline = code.rfind('\n', callPos);
    uint32_t line = 2;
    uint32_t col = static_cast<uint32_t>(callPos - lastNewline - 1);

    auto hover = env.HoverAt(code, line, col);
    REQUIRE(hover.has_value());
    const auto& content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("void " + fnName + "(int val)") != std::string::npos);
    CHECK(content.value.find("overload") == std::string::npos);
    CHECK(content.value.find("sobrecarga") == std::string::npos);
}
