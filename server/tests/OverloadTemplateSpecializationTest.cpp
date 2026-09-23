/**
 * @file OverloadTemplateSpecializationTest.cpp
 * @brief Regression tests for template container overload resolution, intermediate scoped
 *        namespace navigation, and multi-sink overload logging.
 */

#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "features/definition/DefinitionHandler.h"
#include "features/hover/HoverHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/MultiFileLogger.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;
using namespace angel_lsp::features;

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
    if (pos == std::string::npos)
    {
        return {0, 0};
    }
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
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;

    explicit TestEnvironment(const std::string& code)
        : uri("file:///" + test::GenerateRandomSymbolName("doc") + ".as"), sourceCode(code)
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
        HoverRequest req{uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{line, character}};
        return GetHover(req);
    }

    std::optional<std::vector<lsp::Location>> DefAt(uint32_t line, uint32_t character)
    {
        DefinitionRequest req{uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{line, character}};
        return GetDefinition(req);
    }
};

std::string GetHoverText(const lsp::Hover& hover)
{
    if (const auto* content = std::get_if<lsp::MarkupContent>(&hover.contents))
    {
        return content->value;
    }
    return "";
}

Symbol MakeContainerOverload(const std::string& fnName, const std::string& containerParam)
{
    FunctionSignature sig;
    sig.returnType = "void";

    ParameterInformation pJson;
    pJson.name = "input";
    pJson.typeName = "dictionary@";
    pJson.isHandle = true;
    sig.parameters.push_back(pJson);

    ParameterInformation pOut;
    pOut.name = "output";
    pOut.typeName = containerParam;
    pOut.isHandle = true;
    pOut.isReference = true;
    pOut.modifier = ParameterModifier::Out;
    sig.parameters.push_back(pOut);

    Symbol sym;
    sym.name = fnName;
    sym.type = SymbolType::Function;
    sym.signature = sig;
    return sym;
}
} // namespace

TEST_SUITE("OverloadTemplateSpecialization")
{
    /**
     * @brief Test 1: Container template overload resolution selects array<string>@ &out
     *        even when array<float>@ &out and array<int>@ &out are declared first.
     */
    TEST_CASE("OverloadResolver - Selects matching container template over earlier candidates")
    {
        const std::string fnName = test::GenerateRandomSymbolName("ToArray");
        SymbolTable table;

        // Declared first: array<float>@ &out
        Symbol symFloat = MakeContainerOverload(fnName, "array<float>@ &out");
        // Declared second: array<int>@ &out
        Symbol symInt = MakeContainerOverload(fnName, "array<int>@ &out");
        // Declared third: array<string>@ &out
        Symbol symStr = MakeContainerOverload(fnName, "array<string>@ &out");

        const std::vector<Symbol> candidates = {symFloat, symInt, symStr};

        // Caller passes (dictionary@, array<string>) where arg 2 is an lvalue non-handle array<string>
        const std::vector<std::string> argTypes = {"dictionary@", "array<string>"};
        const std::vector<bool> isLValue = {false, true};

        auto match = ResolveBestOverload(candidates, argTypes, table, isLValue);
        REQUIRE(match.bestCandidate != nullptr);
        CHECK(match.bestCandidate->GetFunction().parameters[1].typeName == "array<string>@ &out");
    }

    /**
     * @brief Test 2: Intermediate scoped navigation allows hovering and Ctrl+Click Go-To-Def
     *        across all segments of a multi-level namespace hierarchy.
     */
    TEST_CASE("Namespace - Intermediate scoped navigation and canonical hover")
    {
        const std::string nsA = test::GenerateRandomSymbolName("NsRoot");
        const std::string nsB = test::GenerateRandomSymbolName("NsMid");
        const std::string nsC = test::GenerateRandomSymbolName("NsLeaf");
        const std::string fnWorker = test::GenerateRandomSymbolName("Worker");

        std::string code = "namespace " + nsA + " {\n"
                           "    namespace " + nsB + " {\n"
                           "        namespace " + nsC + " {\n"
                           "            void " + fnWorker + "() {}\n"
                           "        }\n"
                           "    }\n"
                           "}\n\n"
                           "void Invoker() {\n"
                           "    " + nsA + "::" + nsB + "::" + nsC + "::" + fnWorker + "();\n"
                           "}\n";

        TestEnvironment env(code);

        // Hover on nsA in the call
        size_t callPos = code.find(nsA + "::" + nsB + "::" + nsC);
        REQUIRE(callPos != std::string::npos);
        SourcePos posA = FindPos(code, nsA, callPos);
        auto hoverA = env.HoverAt(posA.line, posA.character);
        REQUIRE(hoverA.has_value());
        CHECK(GetHoverText(*hoverA).find("namespace " + nsA) != std::string::npos);

        // Hover on nsB in the call -> expects "namespace nsA::nsB"
        SourcePos posB = FindPos(code, nsB, callPos);
        auto hoverB = env.HoverAt(posB.line, posB.character);
        REQUIRE(hoverB.has_value());
        CHECK(GetHoverText(*hoverB).find("namespace " + nsA + "::" + nsB) != std::string::npos);

        // Hover on nsC in the call -> expects "namespace nsA::nsB::nsC"
        SourcePos posC = FindPos(code, nsC, callPos);
        auto hoverC = env.HoverAt(posC.line, posC.character);
        REQUIRE(hoverC.has_value());
        CHECK(GetHoverText(*hoverC).find("namespace " + nsA + "::" + nsB + "::" + nsC) != std::string::npos);

        // Go-to-Definition on nsB -> navigates to declaration of nsB
        auto defB = env.DefAt(posB.line, posB.character);
        REQUIRE(defB.has_value());
        REQUIRE_FALSE(defB->empty());
        SourcePos declB = FindPos(code, "namespace " + nsB);
        CHECK((*defB)[0].range.start.line == declB.line);

        // Go-to-Definition on nsC -> navigates to declaration of nsC
        auto defC = env.DefAt(posC.line, posC.character);
        REQUIRE(defC.has_value());
        REQUIRE_FALSE(defC->empty());
        SourcePos declC = FindPos(code, "namespace " + nsC);
        CHECK((*defC)[0].range.start.line == declC.line);
    }

    /**
     * @brief Test 3: Sven Co-op pattern with nested namespaces and container output parameters.
     *        Hovering ToArray selects array<string>@ &out and intermediate segments hover accurately.
     */
    TEST_CASE("SvenCoop - Nested namespace ToArray overload resolution and hover")
    {
        const std::string modNs = test::GenerateRandomSymbolName("meta_api");
        const std::string jsonNs = test::GenerateRandomSymbolName("json");
        const std::string v1Ns = test::GenerateRandomSymbolName("v1");
        const std::string fmtNs = test::GenerateRandomSymbolName("fmt");
        const std::string fnToArray = test::GenerateRandomSymbolName("ToArray");
        const std::string arrVar = test::GenerateRandomSymbolName("g_Messages");

        std::string code = "namespace " + modNs + " {\n"
                           "    namespace " + jsonNs + " {\n"
                           "        namespace " + v1Ns + " {\n"
                           "            namespace " + fmtNs + " {\n"
                           "                void " + fnToArray + "(dictionary@ dict, array<float>@ &out) {}\n"
                           "                void " + fnToArray + "(dictionary@ dict, array<string>@ &out) {}\n"
                           "            }\n"
                           "        }\n"
                           "    }\n"
                           "}\n\n"
                           "class TestHost {\n"
                           "    array<string> " + arrVar + ";\n"
                           "    void Run(dictionary@ data) {\n"
                           "        " + modNs + "::" + jsonNs + "::" + v1Ns + "::" + fmtNs + "::" +
                           fnToArray + "(data, " + arrVar + ");\n"
                           "    }\n"
                           "}\n";

        TestEnvironment env(code);

        size_t callPos = code.find(modNs + "::" + jsonNs + "::" + v1Ns);
        REQUIRE(callPos != std::string::npos);

        // Hover on fmt segment -> "namespace modNs::jsonNs::v1Ns::fmtNs"
        SourcePos posFmt = FindPos(code, fmtNs, callPos);
        auto hoverFmt = env.HoverAt(posFmt.line, posFmt.character);
        REQUIRE(hoverFmt.has_value());
        CHECK(GetHoverText(*hoverFmt).find(modNs + "::" + jsonNs + "::" + v1Ns + "::" + fmtNs) != std::string::npos);

        // Hover on ToArray call -> winner must be array<string>@ &out
        SourcePos posFn = FindPos(code, fnToArray, callPos);
        auto hoverFn = env.HoverAt(posFn.line, posFn.character);
        REQUIRE(hoverFn.has_value());
        const std::string hoverContent = GetHoverText(*hoverFn);
        size_t posStr = hoverContent.find("array<string>@ &out");
        size_t posFloat = hoverContent.find("array<float>@ &out");
        CHECK(posStr != std::string::npos);
        CHECK(posFloat != std::string::npos);
        CHECK(posStr < posFloat);
    }

    /**
     * @brief Test 4: Rich Multi-Sink Telemetry logs overload candidate evaluations,
     *        reasons, and winner to overload.log and mirrors to master.log.
     */
    TEST_CASE("MultiFileLogger - Overload channel telemetry and master mirroring")
    {
        const std::string sandboxName = test::GenerateRandomSymbolName("overload_sink_test");
        const std::filesystem::path sandboxDir =
            std::filesystem::temp_directory_path() / sandboxName / ".vscode" / "lsp";

        utils::MultiFileLogger& logger = utils::MultiFileLogger::Instance();
        logger.Initialize(sandboxDir);

        REQUIRE(logger.IsInitialized());
        const auto activeDir = logger.GetActiveLogDirectory();
        CHECK(std::filesystem::exists(activeDir));

        const std::string fnName = test::GenerateRandomSymbolName("ProcessContainer");
        SymbolTable table;

        Symbol symFloat = MakeContainerOverload(fnName, "array<float>@ &out");
        Symbol symStr = MakeContainerOverload(fnName, "array<string>@ &out");
        const std::vector<Symbol> candidates = {symFloat, symStr};

        auto match = ResolveBestOverload(candidates, {"dictionary@", "array<string>"}, table, {false, true});
        REQUIRE(match.bestCandidate != nullptr);

        logger.Flush();

        const auto overloadFile = activeDir / "overload.log";
        const auto masterFile = activeDir / "master.log";

        CHECK(std::filesystem::exists(overloadFile));
        CHECK(std::filesystem::exists(masterFile));

        std::ifstream overloadStream(overloadFile);
        std::string overloadContent((std::istreambuf_iterator<char>(overloadStream)), std::istreambuf_iterator<char>());
        CHECK(overloadContent.find("[OVERLOAD]") != std::string::npos);
        CHECK(overloadContent.find(fnName) != std::string::npos);
        CHECK(overloadContent.find("Winner: " + fnName) != std::string::npos);

        std::ifstream masterStream(masterFile);
        std::string masterContent((std::istreambuf_iterator<char>(masterStream)), std::istreambuf_iterator<char>());
        CHECK(masterContent.find("[OVERLOAD]") != std::string::npos);
        CHECK(masterContent.find("Winner: " + fnName) != std::string::npos);

        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / sandboxName, ec);
    }
}
