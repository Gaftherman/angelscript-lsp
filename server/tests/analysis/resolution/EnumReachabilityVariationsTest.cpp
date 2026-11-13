#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
/**
 * @brief Analyzes an AngelScript snippet and returns all collected and semantic diagnostics.
 * @param[in] code Source code snippet to analyze.
 * @param[in] fileUri Virtual document URI for symbol collection and diagnostic reporting.
 * @return Vector of diagnostics generated during symbol collection and semantic analysis.
 */
std::vector<Diagnostic> AnalyzeCode(const std::string& code,
                                    const std::string& fileUri = "file:///enum_reachability_variations.as")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    auto diagnostics = collector.CollectSymbols(fileUri, code, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    TSTree* tree = parser.Parse(code);
    NodeIndex nodeIndex(ts_tree_root_node(tree));
    request.nodeIndex = &nodeIndex;

    SemanticAnalyzer analyzer(nullptr);
    auto semDiags = analyzer.Analyze(request);
    diagnostics.insert(diagnostics.end(), semDiags.begin(), semDiags.end());

    if (tree)
    {
        ts_tree_delete(tree);
    }
    return diagnostics;
}

/**
 * @brief Determines if any emitted diagnostics indicate enum reachability or undefined identifier failures.
 * @param[in] diagnostics Diagnostics emitted by analysis.
 * @return True if any reachability-related diagnostic is present.
 */
bool HasReachabilityDiagnostic(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d)
                       {
                           return d.code == "as-warn-undeclared-identifier" || d.code == "as-err-undefined-namespace" ||
                                  d.code == "as-err-enum-scope-required";
                       });
}

/**
 * @brief Checks whether the emitted diagnostics match the expected verdict.
 * @param[in] diagnostics Diagnostics emitted by analysis.
 * @param[in] expectedVerdict Expected diagnostic code or "none".
 * @return True if verdict is satisfied.
 */
bool MatchesVerdict(const std::vector<Diagnostic>& diagnostics, const std::string& expectedVerdict)
{
    if (expectedVerdict == "none")
    {
        return !HasReachabilityDiagnostic(diagnostics);
    }
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d) { return d.code == expectedVerdict; });
}
} // namespace

TEST_SUITE("EnumReachabilityVariations")
{
    TEST_CASE("Variation 01 - Fully qualified enum member reached across multi-level nested namespaces")
    {
        const std::string outerNs = angel_lsp::test::GenerateRandomSymbolName("Outer");
        const std::string innerNs = angel_lsp::test::GenerateRandomSymbolName("Inner");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Mode");
        const std::string valInactive = angel_lsp::test::GenerateRandomSymbolName("INACTIVE");
        const std::string valActive = angel_lsp::test::GenerateRandomSymbolName("ACTIVE");
        const std::string runFn = angel_lsp::test::GenerateRandomSymbolName("Run");
        const std::string modeVar = angel_lsp::test::GenerateRandomSymbolName("currentMode");

        const std::string code = "namespace " + outerNs +
                                 " {\n"
                                 "    namespace " +
                                 innerNs +
                                 " {\n"
                                 "        enum " +
                                 enumName +
                                 " {\n"
                                 "            " +
                                 valInactive +
                                 ",\n"
                                 "            " +
                                 valActive +
                                 "\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n"
                                 "void " +
                                 runFn +
                                 "()\n"
                                 "{\n"
                                 "    " +
                                 outerNs + "::" + innerNs + "::" + enumName + " " + modeVar + " = " + outerNs +
                                 "::" + innerNs + "::" + enumName + "::" + valActive +
                                 ";\n"
                                 "    if (" +
                                 modeVar + " == " + outerNs + "::" + innerNs + "::" + enumName + "::" + valActive +
                                 ")\n"
                                 "    {\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 02 - Unqualified enum value accessed from within the same namespace")
    {
        const std::string workerNs = angel_lsp::test::GenerateRandomSymbolName("Worker");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("JobState");
        const std::string valIdle = angel_lsp::test::GenerateRandomSymbolName("IDLE");
        const std::string valBusy = angel_lsp::test::GenerateRandomSymbolName("BUSY");
        const std::string procFn = angel_lsp::test::GenerateRandomSymbolName("Process");
        const std::string stateVar = angel_lsp::test::GenerateRandomSymbolName("state");

        const std::string code = "namespace " + workerNs +
                                 " {\n"
                                 "    enum " +
                                 enumName +
                                 " {\n"
                                 "        " +
                                 valIdle +
                                 ",\n"
                                 "        " +
                                 valBusy +
                                 "\n"
                                 "    }\n"
                                 "    void " +
                                 procFn +
                                 "()\n"
                                 "    {\n"
                                 "        " +
                                 enumName + " " + stateVar + " = " + valIdle +
                                 ";\n"
                                 "        if (" +
                                 stateVar + " == " + valBusy +
                                 ")\n"
                                 "        {\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 03 - Class member compared with an enum from an enclosing namespace")
    {
        const std::string gameNs = angel_lsp::test::GenerateRandomSymbolName("Game");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Difficulty");
        const std::string valEasy = angel_lsp::test::GenerateRandomSymbolName("EASY");
        const std::string valHard = angel_lsp::test::GenerateRandomSymbolName("HARD");
        const std::string matchClass = angel_lsp::test::GenerateRandomSymbolName("Match");
        const std::string diffVar = angel_lsp::test::GenerateRandomSymbolName("currentDifficulty");
        const std::string isHardFn = angel_lsp::test::GenerateRandomSymbolName("IsHard");

        const std::string code = "namespace " + gameNs +
                                 " {\n"
                                 "    enum " +
                                 enumName +
                                 " {\n"
                                 "        " +
                                 valEasy +
                                 ",\n"
                                 "        " +
                                 valHard +
                                 "\n"
                                 "    }\n"
                                 "    class " +
                                 matchClass +
                                 " {\n"
                                 "        " +
                                 enumName + " " + diffVar +
                                 ";\n"
                                 "        bool " +
                                 isHardFn +
                                 "()\n"
                                 "        {\n"
                                 "            return " +
                                 diffVar + " == " + valHard +
                                 ";\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 04 - Enum values reached through using namespace")
    {
        const std::string audioNs = angel_lsp::test::GenerateRandomSymbolName("Audio");
        const std::string mixerNs = angel_lsp::test::GenerateRandomSymbolName("Mixer");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Channel");
        const std::string valLeft = angel_lsp::test::GenerateRandomSymbolName("LEFT");
        const std::string valRight = angel_lsp::test::GenerateRandomSymbolName("RIGHT");
        const std::string setOutFn = angel_lsp::test::GenerateRandomSymbolName("SetOutput");
        const std::string chVar = angel_lsp::test::GenerateRandomSymbolName("ch");

        const std::string code = "namespace " + audioNs +
                                 " {\n"
                                 "    enum " +
                                 enumName +
                                 " {\n"
                                 "        " +
                                 valLeft +
                                 ",\n"
                                 "        " +
                                 valRight +
                                 "\n"
                                 "    }\n"
                                 "}\n"
                                 "namespace " +
                                 mixerNs +
                                 " {\n"
                                 "    using namespace " +
                                 audioNs +
                                 ";\n"
                                 "    void " +
                                 setOutFn +
                                 "()\n"
                                 "    {\n"
                                 "        " +
                                 enumName + " " + chVar + " = " + valLeft +
                                 ";\n"
                                 "        if (" +
                                 chVar + " == " + valRight +
                                 ")\n"
                                 "        {\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 05 - Unqualified enum value reached in an inner nested namespace from parent namespace")
    {
        const std::string engineNs = angel_lsp::test::GenerateRandomSymbolName("Engine");
        const std::string renderNs = angel_lsp::test::GenerateRandomSymbolName("Renderer");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Quality");
        const std::string valLow = angel_lsp::test::GenerateRandomSymbolName("LOW");
        const std::string valHigh = angel_lsp::test::GenerateRandomSymbolName("HIGH");
        const std::string setupFn = angel_lsp::test::GenerateRandomSymbolName("Setup");
        const std::string qVar = angel_lsp::test::GenerateRandomSymbolName("q");

        const std::string code = "namespace " + engineNs +
                                 " {\n"
                                 "    enum " +
                                 enumName +
                                 " {\n"
                                 "        " +
                                 valLow +
                                 ",\n"
                                 "        " +
                                 valHigh +
                                 "\n"
                                 "    }\n"
                                 "    namespace " +
                                 renderNs +
                                 " {\n"
                                 "        void " +
                                 setupFn +
                                 "()\n"
                                 "        {\n"
                                 "            " +
                                 enumName + " " + qVar + " = " + valHigh +
                                 ";\n"
                                 "            if (" +
                                 qVar + " == " + valLow +
                                 ")\n"
                                 "            {\n"
                                 "            }\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 06 - Class member in nested namespace compared with enum from enclosing namespace via mixin")
    {
        const std::string netNs = angel_lsp::test::GenerateRandomSymbolName("Network");
        const std::string clientNs = angel_lsp::test::GenerateRandomSymbolName("Clients");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Protocol");
        const std::string valTcp = angel_lsp::test::GenerateRandomSymbolName("TCP");
        const std::string valUdp = angel_lsp::test::GenerateRandomSymbolName("UDP");
        const std::string mixinClass = angel_lsp::test::GenerateRandomSymbolName("SocketMixin");
        const std::string connClass = angel_lsp::test::GenerateRandomSymbolName("Connection");
        const std::string protoVar = angel_lsp::test::GenerateRandomSymbolName("proto");
        const std::string isTcpFn = angel_lsp::test::GenerateRandomSymbolName("IsTcp");
        const std::string initFn = angel_lsp::test::GenerateRandomSymbolName("Init");

        const std::string code = "namespace " + netNs +
                                 " {\n"
                                 "    enum " +
                                 enumName +
                                 " {\n"
                                 "        " +
                                 valTcp +
                                 ",\n"
                                 "        " +
                                 valUdp +
                                 "\n"
                                 "    }\n"
                                 "    mixin class " +
                                 mixinClass +
                                 " {\n"
                                 "        " +
                                 enumName + " " + protoVar +
                                 ";\n"
                                 "        bool " +
                                 isTcpFn +
                                 "()\n"
                                 "        {\n"
                                 "            return " +
                                 protoVar + " == " + valTcp +
                                 ";\n"
                                 "        }\n"
                                 "    }\n"
                                 "    namespace " +
                                 clientNs +
                                 " {\n"
                                 "        class " +
                                 connClass + " : " + mixinClass +
                                 " {\n"
                                 "            void " +
                                 initFn +
                                 "()\n"
                                 "            {\n"
                                 "                " +
                                 protoVar + " = " + valUdp +
                                 ";\n"
                                 "            }\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 07 - Unqualified enum value from unrelated foreign namespace without using namespace")
    {
        const std::string sensorNs = angel_lsp::test::GenerateRandomSymbolName("Sensors");
        const std::string dashNs = angel_lsp::test::GenerateRandomSymbolName("Dashboard");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("AlarmLevel");
        const std::string valSafe = angel_lsp::test::GenerateRandomSymbolName("SAFE");
        const std::string valAlert = angel_lsp::test::GenerateRandomSymbolName("ALERT");
        const std::string refreshFn = angel_lsp::test::GenerateRandomSymbolName("Refresh");
        const std::string levelVar = angel_lsp::test::GenerateRandomSymbolName("level");

        const std::string code = "namespace " + sensorNs +
                                 " {\n"
                                 "    enum " +
                                 enumName +
                                 " {\n"
                                 "        " +
                                 valSafe +
                                 ",\n"
                                 "        " +
                                 valAlert +
                                 "\n"
                                 "    }\n"
                                 "}\n"
                                 "namespace " +
                                 dashNs +
                                 " {\n"
                                 "    void " +
                                 refreshFn +
                                 "()\n"
                                 "    {\n"
                                 "        int " +
                                 levelVar + " = " + valAlert +
                                 ";\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-undeclared-identifier";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 08 - Enum member accessed with undefined namespace qualifier prefix")
    {
        const std::string sysNs = angel_lsp::test::GenerateRandomSymbolName("System");
        const std::string unknownNs = angel_lsp::test::GenerateRandomSymbolName("UnknownNS");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Status");
        const std::string valOffline = angel_lsp::test::GenerateRandomSymbolName("OFFLINE");
        const std::string valReady = angel_lsp::test::GenerateRandomSymbolName("READY");
        const std::string checkFn = angel_lsp::test::GenerateRandomSymbolName("Check");
        const std::string sVar = angel_lsp::test::GenerateRandomSymbolName("s");

        const std::string code = "namespace " + sysNs +
                                 " {\n"
                                 "    enum " +
                                 enumName +
                                 " {\n"
                                 "        " +
                                 valOffline +
                                 ",\n"
                                 "        " +
                                 valReady +
                                 "\n"
                                 "    }\n"
                                 "}\n"
                                 "void " +
                                 checkFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 sVar + " = " + unknownNs + "::" + enumName + "::" + valReady +
                                 ";\n"
                                 "}\n";

        const std::string expectedVerdict = "as-err-undefined-namespace";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 09 - Parent namespace attempting to access unqualified enum value in child namespace")
    {
        const std::string gfxNs = angel_lsp::test::GenerateRandomSymbolName("Graphics");
        const std::string shaderNs = angel_lsp::test::GenerateRandomSymbolName("Shader");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Stage");
        const std::string valVert = angel_lsp::test::GenerateRandomSymbolName("VERTEX");
        const std::string valFrag = angel_lsp::test::GenerateRandomSymbolName("FRAGMENT");
        const std::string compileFn = angel_lsp::test::GenerateRandomSymbolName("Compile");
        const std::string sVar = angel_lsp::test::GenerateRandomSymbolName("s");

        const std::string code = "namespace " + gfxNs +
                                 " {\n"
                                 "    namespace " +
                                 shaderNs +
                                 " {\n"
                                 "        enum " +
                                 enumName +
                                 " {\n"
                                 "            " +
                                 valVert +
                                 ",\n"
                                 "            " +
                                 valFrag +
                                 "\n"
                                 "        }\n"
                                 "    }\n"
                                 "    void " +
                                 compileFn +
                                 "()\n"
                                 "    {\n"
                                 "        int " +
                                 sVar + " = " + valVert +
                                 ";\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-undeclared-identifier";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasReachabilityDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 10 - Sibling nested namespace attempting to access unqualified enum value from sibling")
    {
        const std::string appNs = angel_lsp::test::GenerateRandomSymbolName("App");
        const std::string inputNs = angel_lsp::test::GenerateRandomSymbolName("Input");
        const std::string displayNs = angel_lsp::test::GenerateRandomSymbolName("Display");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Key");
        const std::string valEnter = angel_lsp::test::GenerateRandomSymbolName("ENTER");
        const std::string valEscape = angel_lsp::test::GenerateRandomSymbolName("ESCAPE");
        const std::string renderFn = angel_lsp::test::GenerateRandomSymbolName("Render");
        const std::string kVar = angel_lsp::test::GenerateRandomSymbolName("k");

        const std::string code = "namespace " + appNs +
                                 " {\n"
                                 "    namespace " +
                                 inputNs +
                                 " {\n"
                                 "        enum " +
                                 enumName +
                                 " {\n"
                                 "            " +
                                 valEnter +
                                 ",\n"
                                 "            " +
                                 valEscape +
                                 "\n"
                                 "        }\n"
                                 "    }\n"
                                 "    namespace " +
                                 displayNs +
                                 " {\n"
                                 "        void " +
                                 renderFn +
                                 "()\n"
                                 "        {\n"
                                 "            int " +
                                 kVar + " = " + valEscape +
                                 ";\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-undeclared-identifier";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasReachabilityDiagnostic(diagnostics));
    }
}
