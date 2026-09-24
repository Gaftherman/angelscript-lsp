#include <doctest/doctest.h>

#include "analysis/Diagnostics.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include "analysis/ScopeTree.h"
#include "features/completion/CompletionHandler.h"
#include "features/hover/HoverHandler.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
/**
 * @brief Test fixture loading sven.as.predefined for empirical end-to-end LSP verification.
 */
struct SvenTestContext
{
    AngelScriptParser parser;
    SymbolCollector collector{nullptr};
    LocalScopeCollector scopes{nullptr};
    SymbolTable table;
    ScopeIndex scopeIndex;
    angel_lsp::i18n::I18n i18n;

    /**
     * @brief Constructs the test context and preloads symbols from sven.as.predefined.
     */
    SvenTestContext()
    {
        std::filesystem::path repoRoot(ANGELSCRIPT_REPO_ROOT);
        std::filesystem::path stubPath = repoRoot / "predefined" / "sven.as.predefined";
        REQUIRE_MESSAGE(std::filesystem::exists(stubPath), "sven.as.predefined must exist");

        std::ifstream stubFile(stubPath, std::ios::binary);
        REQUIRE(stubFile.is_open());
        std::ostringstream ss;
        ss << stubFile.rdbuf();
        const std::string svenStub = ss.str();

        collector.CollectSymbols("file:///sven.as.predefined", svenStub, parser, table);
    }

    /**
     * @brief Parses and semantically analyzes the given script code.
     * @param[in] scriptCode AngelScript source string.
     * @param[in] fileUri Virtual document URI.
     * @return List of generated diagnostics.
     */
    std::vector<Diagnostic> Analyze(const std::string& scriptCode,
                                    const std::string& fileUri = "file:///IsPluginInstalled.as")
    {
        collector.CollectSymbols(fileUri, scriptCode, parser, table);

        SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
        request.scopeRoot = scopes.CollectScopes(scriptCode, parser);
        request.sourceCode = scriptCode;
        request.tree = parser.Parse(scriptCode);

        SemanticAnalyzer analyzer(nullptr);
        auto diags = analyzer.Analyze(request);
        if (request.tree)
        {
            ts_tree_delete(const_cast<TSTree*>(request.tree));
        }
        return diags;
    }

    /**
     * @brief Evaluates hover tooltip information at the specified position.
     * @param[in] scriptCode AngelScript source string.
     * @param[in] line 0-indexed line number.
     * @param[in] character 0-indexed character offset.
     * @param[in] fileUri Virtual document URI.
     * @return Optional hover result.
     */
    std::optional<lsp::Hover> HoverAt(const std::string& scriptCode, uint32_t line, uint32_t character,
                                      const std::string& fileUri = "file:///IsPluginInstalled.as")
    {
        collector.CollectSymbols(fileUri, scriptCode, parser, table);
        auto rootScope = scopes.CollectScopes(scriptCode, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(fileUri, std::move(rootScope));
        }
        TSTree* tree = parser.Parse(scriptCode);
        angel_lsp::features::HoverRequest req{fileUri, scriptCode, tree,
                                              table,   scopeIndex, lsp::Position{line, character}};
        auto res = angel_lsp::features::GetHover(req);
        if (tree)
        {
            ts_tree_delete(tree);
        }
        return res;
    }

    /**
     * @brief Computes completion candidates at the specified cursor position.
     * @param[in] scriptCode AngelScript source string.
     * @param[in] line 0-indexed line number.
     * @param[in] character 0-indexed character offset.
     * @param[in] fileUri Virtual document URI.
     * @return Vector of completion items.
     */
    std::vector<lsp::CompletionItem> CompleteAt(const std::string& scriptCode, uint32_t line, uint32_t character,
                                                const std::string& fileUri = "file:///IsPluginInstalled.as")
    {
        collector.CollectSymbols(fileUri, scriptCode, parser, table);
        auto rootScope = scopes.CollectScopes(scriptCode, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(fileUri, std::move(rootScope));
        }
        TSTree* tree = parser.Parse(scriptCode);
        angel_lsp::features::CompletionRequest req{fileUri, scriptCode, tree,
                                                   table,   scopeIndex, lsp::Position{line, character}};
        auto res = angel_lsp::features::GetCompletion(req);
        if (tree)
        {
            ts_tree_delete(tree);
        }
        return res;
    }
};

/**
 * @brief Checks if a diagnostic list contains a diagnostic with the given error code.
 * @param[in] diags List of diagnostics.
 * @param[in] code Diagnostic code string to check.
 * @return True if code is found.
 */
bool HasCode(const std::vector<Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(), [&](const Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_CASE("SvenCoopScriptReproduction - IsPluginInstalled valid script emits zero diagnostics")
{
    SvenTestContext ctx;

    const std::string script = R"AS(
namespace Server
{
    bool IsPluginInstalled( const string&in plugin_name, bool case_sensitive = false )
    {
        array<string>@ pluginList = g_PluginManager.GetPluginList();
        if( case_sensitive )
        {
            return ( pluginList.find( plugin_name ) >= 0 );
        }
        string plugin_name_lowercase = plugin_name.ToLowercase();
        for( uint ui = 0; ui < pluginList.length(); ui++ )
        {
            if( pluginList[ui].ToLowercase() == plugin_name_lowercase )
            {
                return true;
            }
        }
        return false;
    }
}
)AS";

    auto diags = ctx.Analyze(script);
    for (const auto& d : diags)
    {
        MESSAGE("Test 1 diagnostic: " << d.code << " at line " << d.range.start.line << ": " << d.message);
    }
    CHECK(diags.empty());
}

TEST_CASE("SvenCoopScriptReproduction - Invalid member call on array element emits diagnostic")
{
    SvenTestContext ctx;

    const std::string script = R"AS(
void TestInvalidAccess()
{
    array<string>@ list = g_PluginManager.GetPluginList();
    list[0].NonExistentMethodCall();
}
)AS";

    auto diags = ctx.Analyze(script, "file:///TestInvalidAccess.as");
    for (const auto& d : diags)
    {
        MESSAGE("Test 2 diagnostic: " << d.code << ": " << d.message);
    }
    bool hasMemberError = HasCode(diags, "as-err-no-matching-symbol") || HasCode(diags, "as-err-not-a-member") ||
                          HasCode(diags, "as-err-call-no-matching-signature") ||
                          HasCode(diags, "as-err-member-not-found");
    CHECK(hasMemberError);
}

TEST_CASE("SvenCoopScriptReproduction - Hover on global identifier inside namespace")
{
    SvenTestContext ctx;
    const std::string script = "namespace Server\n"
                               "{\n"
                               "    void Test()\n"
                               "    {\n"
                               "        auto p = g_PluginManager.GetPluginList();\n"
                               "    }\n"
                               "}\n";

    auto hover = ctx.HoverAt(script, 4, 20);
    REQUIRE(hover.has_value());
    const auto* content = std::get_if<lsp::MarkupContent>(&hover->contents);
    REQUIRE(content != nullptr);
    CHECK(content->value.find("g_PluginManager") != std::string::npos);
}

TEST_CASE("SvenCoopScriptReproduction - Hover and Completion on subscripted array element")
{
    SvenTestContext ctx;
    const std::string script = "void Test()\n"
                               "{\n"
                               "    array<string>@ pluginList = g_PluginManager.GetPluginList();\n"
                               "    string s = pluginList[0].ToLowercase();\n"
                               "}\n";

    auto hover = ctx.HoverAt(script, 3, 32);
    REQUIRE(hover.has_value());
    const auto* content = std::get_if<lsp::MarkupContent>(&hover->contents);
    REQUIRE(content != nullptr);
    CHECK(content->value.find("ToLowercase") != std::string::npos);

    const std::string completionScript = "void Test()\n"
                                         "{\n"
                                         "    array<string>@ pluginList = g_PluginManager.GetPluginList();\n"
                                         "    pluginList[0].\n"
                                         "}\n";

    auto items = ctx.CompleteAt(completionScript, 3, 18);
    CHECK(!items.empty());
    bool hasToLowercase = std::any_of(items.begin(), items.end(),
                                      [](const lsp::CompletionItem& item) { return item.label == "ToLowercase"; });
    CHECK(hasToLowercase);
}

TEST_CASE("SvenCoopScriptReproduction - Hover and Completion on typed lambda parameter and member access")
{
    SvenTestContext ctx;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("MapActivate");
    const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("player");
    const std::string script = "void " + fnName + "()\n"
                               "{\n"
                               "    g_Hooks.RegisterHook( Hooks::Player::PlayerPostThink,\n"
                               "    @PlayerPostThinkHook( function( CBasePlayer@ " + paramName + " ) {\n"
                               "        if( " + paramName + " !is null ) {\n"
                               "            " + paramName + ".ResetOverriddenPlayerModel( true, true );\n"
                               "            " + paramName + ".SetOverriddenPlayerModel( g_EngineFuncs.GetInfoKeyBuffer( " + paramName + ".edict() ).GetValue( \"model\" ) );\n"
                               "        }\n"
                               "        return HOOK_CONTINUE;\n"
                               "    } ) );\n"
                               "}\n";

    size_t resetPos = script.find("ResetOverriddenPlayerModel");
    REQUIRE(resetPos != std::string::npos);
    size_t lastNewline = script.rfind('\n', resetPos);
    uint32_t resetCol = static_cast<uint32_t>(resetPos - lastNewline - 1);

    auto hoverReset = ctx.HoverAt(script, 5, resetCol + 4);
    REQUIRE(hoverReset.has_value());
    const auto* contentReset = std::get_if<lsp::MarkupContent>(&hoverReset->contents);
    REQUIRE(contentReset != nullptr);
    CHECK(contentReset->value.find("ResetOverriddenPlayerModel") != std::string::npos);

    size_t setPos = script.find("SetOverriddenPlayerModel");
    REQUIRE(setPos != std::string::npos);
    lastNewline = script.rfind('\n', setPos);
    uint32_t setCol = static_cast<uint32_t>(setPos - lastNewline - 1);

    auto hoverSet = ctx.HoverAt(script, 6, setCol + 4);
    REQUIRE(hoverSet.has_value());
    const auto* contentSet = std::get_if<lsp::MarkupContent>(&hoverSet->contents);
    REQUIRE(contentSet != nullptr);
    CHECK(contentSet->value.find("SetOverriddenPlayerModel") != std::string::npos);

    size_t edictPos = script.find("edict()");
    REQUIRE(edictPos != std::string::npos);
    lastNewline = script.rfind('\n', edictPos);
    uint32_t edictCol = static_cast<uint32_t>(edictPos - lastNewline - 1);

    auto hoverEdict = ctx.HoverAt(script, 6, edictCol + 2);
    REQUIRE(hoverEdict.has_value());
    const auto* contentEdict = std::get_if<lsp::MarkupContent>(&hoverEdict->contents);
    REQUIRE(contentEdict != nullptr);
    CHECK(contentEdict->value.find("edict") != std::string::npos);

    auto hoverParam = ctx.HoverAt(script, 4, 13);
    REQUIRE(hoverParam.has_value());
    const auto* contentParam = std::get_if<lsp::MarkupContent>(&hoverParam->contents);
    REQUIRE(contentParam != nullptr);
    CHECK(contentParam->value.find("CBasePlayer") != std::string::npos);

    const std::string compScript = "void " + fnName + "()\n"
                                   "{\n"
                                   "    g_Hooks.RegisterHook( Hooks::Player::PlayerPostThink,\n"
                                   "    @PlayerPostThinkHook( function( CBasePlayer@ " + paramName + " ) {\n"
                                   "        " + paramName + ".\n"
                                   "    } ) );\n"
                                   "}\n";
    size_t dotPos = compScript.find(paramName + ".");
    REQUIRE(dotPos != std::string::npos);
    lastNewline = compScript.rfind('\n', dotPos);
    uint32_t compCol = static_cast<uint32_t>(dotPos + paramName.length() + 1 - lastNewline - 1);

    auto items = ctx.CompleteAt(compScript, 4, compCol);
    CHECK(!items.empty());
    bool hasReset = std::any_of(items.begin(), items.end(),
                                [](const lsp::CompletionItem& item) { return item.label == "ResetOverriddenPlayerModel"; });
    bool hasSet = std::any_of(items.begin(), items.end(),
                              [](const lsp::CompletionItem& item) { return item.label == "SetOverriddenPlayerModel"; });
    bool hasEdict = std::any_of(items.begin(), items.end(),
                                [](const lsp::CompletionItem& item) { return item.label == "edict"; });
    CHECK(hasReset);
    CHECK(hasSet);
    CHECK(hasEdict);
}

TEST_CASE("SvenCoopScriptReproduction - Hover and Completion on untyped lambda parameter deduced from funcdef")
{
    SvenTestContext ctx;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("MapActivateUntyped");
    const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("player");
    const std::string script = "void " + fnName + "()\n"
                               "{\n"
                               "    g_Hooks.RegisterHook( Hooks::Player::PlayerPostThink,\n"
                               "    @PlayerPostThinkHook( function( " + paramName + " ) {\n"
                               "        " + paramName + ".ResetOverriddenPlayerModel( true, true );\n"
                               "        return HOOK_CONTINUE;\n"
                               "    } ) );\n"
                               "}\n";

    size_t resetPos = script.find("ResetOverriddenPlayerModel");
    REQUIRE(resetPos != std::string::npos);
    size_t lastNewline = script.rfind('\n', resetPos);
    uint32_t resetCol = static_cast<uint32_t>(resetPos - lastNewline - 1);

    auto hoverReset = ctx.HoverAt(script, 4, resetCol + 4);
    REQUIRE(hoverReset.has_value());
    const auto* contentReset = std::get_if<lsp::MarkupContent>(&hoverReset->contents);
    REQUIRE(contentReset != nullptr);
    CHECK(contentReset->value.find("ResetOverriddenPlayerModel") != std::string::npos);

    const std::string compScript = "void " + fnName + "()\n"
                                   "{\n"
                                   "    g_Hooks.RegisterHook( Hooks::Player::PlayerPostThink,\n"
                                   "    @PlayerPostThinkHook( function( " + paramName + " ) {\n"
                                   "        " + paramName + ".\n"
                                   "    } ) );\n"
                                   "}\n";
    size_t dotPos = compScript.find(paramName + ".");
    REQUIRE(dotPos != std::string::npos);
    lastNewline = compScript.rfind('\n', dotPos);
    uint32_t compCol = static_cast<uint32_t>(dotPos + paramName.length() + 1 - lastNewline - 1);

    auto items = ctx.CompleteAt(compScript, 4, compCol);
    CHECK(!items.empty());
    bool hasReset = std::any_of(items.begin(), items.end(),
                                [](const lsp::CompletionItem& item) { return item.label == "ResetOverriddenPlayerModel"; });
    CHECK(hasReset);
}

TEST_CASE("SvenCoopScriptReproduction - User Sven Co-op plugin snippet has zero diagnostics")
{
    SvenTestContext ctx;
    const std::string script = R"AS(
void PluginInit()
{
    g_Module.ScriptInfo.SetAuthor( "Mikk" );
    g_Module.ScriptInfo.SetContactInfo( "https://github.com/Mikk155/Sven-Co-op" );
    MapActivate();
}

void MapActivate()
{
    if( g_Map.HasForcedPlayerModels() )
    {
        g_Hooks.RegisterHook( Hooks::Player::PlayerPostThink,
        @PlayerPostThinkHook( function( CBasePlayer@ player ) {
            if( player !is null ) {
                player.ResetOverriddenPlayerModel( true, true );
                player.SetOverriddenPlayerModel( g_EngineFuncs.GetInfoKeyBuffer( player.edict() ).GetValue( "model" ) );
            }
            return HOOK_CONTINUE;
        } ) );
    }
    else
        g_Hooks.RemoveHook( Hooks::Player::PlayerPostThink );
}
)AS";

    auto diags = ctx.Analyze(script, "file:///UserSnippet.as");
    for (const auto& d : diags)
    {
        MESSAGE("User snippet diagnostic: " << d.code << " at line " << d.range.start.line << ": " << d.message);
    }
    CHECK(diags.empty());
}
