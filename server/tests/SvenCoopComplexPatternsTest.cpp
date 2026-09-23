#include <doctest/doctest.h>

#include "analysis/Diagnostics.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "features/hover/HoverHandler.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "lsp/PredefinedStubManager.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
/**
 * @brief Test context with preloaded sven.as.predefined for real-world validation.
 */
struct ComplexPatternsTestContext
{
    AngelScriptParser parser;
    SymbolCollector collector{nullptr};
    LocalScopeCollector scopes{nullptr};
    SymbolTable table;
    ScopeIndex scopeIndex;
    angel_lsp::i18n::I18n i18n;

    /**
     * @brief Constructs test harness and parses sven.as.predefined if available.
     */
    ComplexPatternsTestContext()
    {
        std::filesystem::path repoRoot(ANGELSCRIPT_REPO_ROOT);
        std::filesystem::path stubPath = repoRoot / "predefined" / "sven.as.predefined";
        if (std::filesystem::exists(stubPath))
        {
            std::ifstream stubFile(stubPath, std::ios::binary);
            if (stubFile.is_open())
            {
                std::ostringstream ss;
                ss << stubFile.rdbuf();
                collector.CollectSymbols("file:///sven.as.predefined", ss.str(), parser, table);
            }
        }
    }

    /**
     * @brief Analyzes code snippet and returns generated diagnostics.
     * @param[in] scriptCode Source snippet to analyze.
     * @param[in] fileUri Virtual document URI.
     * @return Vector of emitted diagnostics.
     */
    std::vector<Diagnostic> Analyze(const std::string& scriptCode,
                                    const std::string& fileUri = "file:///ComplexPatternsTest.as")
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
     * @brief Evaluates hover at a specific position.
     * @param[in] scriptCode Source snippet.
     * @param[in] line 0-indexed line.
     * @param[in] character 0-indexed character.
     * @param[in] fileUri Virtual document URI.
     * @return Optional hover result.
     */
    std::optional<lsp::Hover> HoverAt(const std::string& scriptCode, uint32_t line, uint32_t character,
                                                 const std::string& fileUri = "file:///ComplexPatternsHover.as")
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
};

/**
 * @brief Checks if a diagnostic code is present in the list.
 * @param[in] diags Vector of diagnostics.
 * @param[in] code Diagnostic code string.
 * @return True if code is found.
 */
bool HasCode(const std::vector<Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(), [code](const Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_CASE("SvenCoopComplexPatterns - Predefined Stub Path Deduplication")
{
    angel_lsp::PredefinedStubManager stubManager;
    std::filesystem::path repoRoot(ANGELSCRIPT_REPO_ROOT);
    std::filesystem::path absPath = repoRoot / "predefined" / "sven.as.predefined";

    if (std::filesystem::exists(absPath))
    {
        std::error_code relEc;
        const std::filesystem::path relPath = std::filesystem::relative(absPath, relEc);
        const std::filesystem::path dotPath = repoRoot / "predefined" / "." / ".." / "predefined" / "sven.as.predefined";

        const std::string norm1 = angel_lsp::PredefinedStubManager::CanonicalizeStubPath(relEc ? absPath.string() : relPath.string());
        const std::string norm2 = angel_lsp::PredefinedStubManager::CanonicalizeStubPath(absPath.string());
        const std::string norm3 = angel_lsp::PredefinedStubManager::CanonicalizeStubPath(dotPath.string());
        CHECK(norm1 == norm2);
        CHECK(norm2 == norm3);
        CHECK_FALSE(stubManager.IsCanonicalPathLoaded(norm1));
        CHECK(stubManager.MarkCanonicalPathLoaded(norm1));
        CHECK(stubManager.IsCanonicalPathLoaded(norm2));
        CHECK(stubManager.IsCanonicalPathLoaded(norm3));
        CHECK_FALSE(stubManager.MarkCanonicalPathLoaded(norm2));
        CHECK_FALSE(stubManager.MarkCanonicalPathLoaded(norm3));
    }
}

TEST_CASE("SvenCoopComplexPatterns - Cyclic Auto False Positive Avoidance")
{
    ComplexPatternsTestContext ctx;
    const std::string pmoveCls = angel_lsp::test::GenerateRandomSymbolName("Pmove");
    const std::string playerMember = angel_lsp::test::GenerateRandomSymbolName("player");
    const std::string funcsCls = angel_lsp::test::GenerateRandomSymbolName("PlayerFuncs");
    const std::string globalFuncs = angel_lsp::test::GenerateRandomSymbolName("g_PlayerFuncs");
    const std::string cyclicVar = angel_lsp::test::GenerateRandomSymbolName("selfRef");

    const std::string code =
        "class " + pmoveCls + " {\n"
        "    int " + playerMember + ";\n"
        "}\n"
        "class " + funcsCls + " {\n"
        "    int FindPlayerByIndex(int idx) { return idx; }\n"
        "}\n"
        + funcsCls + " " + globalFuncs + ";\n"
        "void TestMemberAccess(" + pmoveCls + "@ pmove) {\n"
        "    auto " + playerMember + " = " + globalFuncs + ".FindPlayerByIndex(pmove." + playerMember + ");\n"
        "    auto key = \"literal_key\";\n"
        "    auto val = key;\n"
        "}\n"
        "void TestCyclic() {\n"
        "    auto " + cyclicVar + " = " + cyclicVar + " + 1;\n"
        "}\n";

    auto diags = ctx.Analyze(code);
    CHECK_FALSE(HasCode(diags, "as-err-auto-requires-initializer"));

    bool foundGenuineCyclic = false;
    for (const auto& d : diags)
    {
        if (d.code == "as-err-cyclic-auto-dependency")
        {
            if (d.message.find(cyclicVar) != std::string::npos)
            {
                foundGenuineCyclic = true;
            }
            CHECK(d.message.find(playerMember) == std::string::npos);
        }
    }
    CHECK(foundGenuineCyclic);
}

TEST_CASE("SvenCoopComplexPatterns - Instance Member vs Enum Constant Collision")
{
    ComplexPatternsTestContext ctx;
    const std::string loggerCls = angel_lsp::test::GenerateRandomSymbolName("Logger");
    const std::string errorName = angel_lsp::test::GenerateRandomSymbolName("Error");
    const std::string msgVar = angel_lsp::test::GenerateRandomSymbolName("msg");

    const std::string code =
        "class " + loggerCls + " {\n"
        "    enum LogLevel {\n"
        "        None,\n"
        "        Info,\n"
        "        Warning,\n"
        "        " + errorName + "\n"
        "    };\n"
        "    void " + errorName + "(const string& in " + msgVar + ") {}\n"
        "    void LogEvent() {\n"
        "        this." + errorName + "(\"test message\");\n"
        "        " + errorName + "(\"direct call\");\n"
        "    }\n"
        "}\n";

    auto diags = ctx.Analyze(code);
    for (const auto& d : diags)
    {
        CHECK(d.code != "as-err-member-not-found");
        CHECK(d.code != "as-err-call-argument-count");
        CHECK(d.code != "as-err-no-matching-signature");
    }
}

TEST_CASE("SvenCoopComplexPatterns - Anonymous Function Lambdas Callback Inference")
{
    ComplexPatternsTestContext ctx;
    const std::string cmdCls = angel_lsp::test::GenerateRandomSymbolName("CCommand");
    const std::string cbFuncdef = angel_lsp::test::GenerateRandomSymbolName("ClientCommandCallback");
    const std::string clientCmdCls = angel_lsp::test::GenerateRandomSymbolName("CClientCommand");
    const std::string regFunc = angel_lsp::test::GenerateRandomSymbolName("RegisterCommands");

    const std::string code =
        "class " + cmdCls + " {};\n"
        "funcdef void " + cbFuncdef + "(const " + cmdCls + "@ args);\n"
        "class " + clientCmdCls + " {\n"
        "    " + clientCmdCls + "(const string& in name, const string& in desc, " + cbFuncdef + "@ cb) {}\n"
        "}\n"
        "void " + regFunc + "() {\n"
        "    " + clientCmdCls + "(\"cmd\", \"desc\", function(const " + cmdCls + "@ args) {\n"
        "        // anonymous callback\n"
        "    });\n"
        "    " + cbFuncdef + "@ localCb = function(const " + cmdCls + "@ args) {};\n"
        "}\n";

    auto diags = ctx.Analyze(code);
    for (const auto& d : diags)
    {
        CHECK(d.message.find("function") == std::string::npos);
        CHECK(d.code != "as-warn-undeclared-identifier");
        CHECK(d.code != "as-err-call-no-matching-signature");
    }
}

TEST_CASE("SvenCoopComplexPatterns - Scoped Identifier Segment Hover & Property DocComments")
{
    ComplexPatternsTestContext ctx;
    const std::string outerNs = angel_lsp::test::GenerateRandomSymbolName("OuterNs");
    const std::string innerNs = angel_lsp::test::GenerateRandomSymbolName("InnerNs");
    const std::string workerCls = angel_lsp::test::GenerateRandomSymbolName("Worker");
    const std::string entityCls = angel_lsp::test::GenerateRandomSymbolName("Entity");
    const std::string hpProp = angel_lsp::test::GenerateRandomSymbolName("hp");

    const std::string code =
        "namespace " + outerNs + " {\n"
        "    namespace " + innerNs + " {\n"
        "        class " + workerCls + " {\n"
        "            void Work() {}\n"
        "        }\n"
        "    }\n"
        "}\n"
        "class " + entityCls + " {\n"
        "    /** @brief The entity health score. */\n"
        "    int get_" + hpProp + "() property { return 100; }\n"
        "}\n"
        "void Runner() {\n"
        "    " + outerNs + "::" + innerNs + "::" + workerCls + " w;\n"
        "    " + entityCls + " e;\n"
        "    int val = e." + hpProp + ";\n"
        "}\n";

    // Hover on 'InnerNs' in 'OuterNs::InnerNs::Worker w;'
    // Line 12: "    " + outerNs + "::" + innerNs + "::" + workerCls + " w;\n"
    const size_t innerCol = 4 + outerNs.size() + 2;
    auto hoverInner = ctx.HoverAt(code, 12, static_cast<uint32_t>(innerCol));
    REQUIRE(hoverInner.has_value());
    CHECK(hoverInner->range.has_value());
    CHECK(hoverInner->range->start.character == innerCol);
    CHECK(hoverInner->range->end.character == innerCol + innerNs.size());

    // Hover on 'hp' property in 'e.hp'
    // Line 14: "    int val = e." + hpProp + ";\n"
    const size_t hpCol = 4 + 12; // "    int val = e."
    auto hoverHp = ctx.HoverAt(code, 14, static_cast<uint32_t>(hpCol));
    REQUIRE(hoverHp.has_value());
    auto content = std::get<lsp::MarkupContent>(hoverHp->contents);
    CHECK(content.value.find("The entity health score.") != std::string::npos);
}
