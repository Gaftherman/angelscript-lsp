/**
 * @file JsonParityIncludeTest.cpp
 * @brief Invariant-based test suite verifying qualified Go-To-Def, scoped enum resolution,
 *        relative and root type lookup, extensionless include path traversal, preprocessor scope anchoring,
 *        CFG switch exhaustiveness, and implicit default constructor synthesis.
 */

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "features/definition/DefinitionHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/IncludeResolver.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::test;

namespace
{
/**
 * @brief Analyzes a script snippet and returns collected diagnostics.
 * @param[in] code AngelScript source text.
 * @param[in] fileUri Virtual document URI.
 * @return Vector of emitted diagnostics.
 */
std::vector<Diagnostic> AnalyzeSnippet(const std::string& code, const std::string& fileUri = "file:///test.as")
{
    parser::AngelScriptParser parser;
    SymbolTable table;
    SymbolCollector collector(nullptr);
    auto diags = collector.CollectSymbols(fileUri, code, parser, table);

    LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(code, parser);

    TSTree* tree = parser.Parse(code);

    SemanticAnalyzer analyzer(nullptr);
    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", nullptr};
    request.sourceCode = code;
    request.tree = tree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());
    if (tree)
    {
        ts_tree_delete(tree);
    }
    return diags;
}

/**
 * @brief Checks if any diagnostic matches the given code.
 * @param[in] diags List of diagnostics.
 * @param[in] code Diagnostic code string view.
 * @return True if matched.
 */
bool HasDiagCode(const std::vector<Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(), [code](const Diagnostic& d) { return d.code == code; });
}

struct SourcePos
{
    uint32_t line = 0;
    uint32_t character = 0;
};

/**
 * @brief Finds the line and character offset of a needle in source text.
 * @param[in] source Document text.
 * @param[in] needle Search string.
 * @param[in] startAt Byte search start offset.
 * @return Source position structure.
 */
SourcePos FindPos(const std::string& source, const std::string& needle, size_t startAt = 0)
{
    const size_t pos = source.find(needle, startAt);
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

struct WorkspaceDoc
{
    std::string uri;
    std::string code;
};

/**
 * @brief Analyzes a document within a shared workspace symbol table context.
 * @param[in,out] table Shared symbol table.
 * @param[in] doc Workspace document descriptor.
 * @param[in,out] parser AngelScript parser instance.
 * @param[out] outTree Parsed AST tree handle.
 * @return Emitted diagnostics across symbol collection and semantic analysis.
 */
std::vector<Diagnostic> AnalyzeWorkspaceDoc(SymbolTable& table, const WorkspaceDoc& doc,
                                            parser::AngelScriptParser& parser, TSTree*& outTree)
{
    SymbolCollector collector(nullptr);
    auto diags = collector.CollectSymbols(doc.uri, doc.code, parser, table);

    LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(doc.code, parser);

    outTree = parser.Parse(doc.code);

    SemanticAnalyzer analyzer(nullptr);
    SemanticAnalysisRequest request{table, doc.uri, ".as.predefined", nullptr};
    request.sourceCode = doc.code;
    request.tree = outTree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());
    return diags;
}

/**
 * @brief Asserts that a Go-To-Definition query resolves to the expected document URI.
 * @param[in] req Definition query request.
 * @param[in] expectedUri Expected target URI.
 */
void AssertDefinitionTarget(const features::DefinitionRequest& req, const std::string& expectedUri)
{
    const auto result = features::GetDefinition(req);
    REQUIRE(result.has_value());
    REQUIRE(!result->empty());
    CHECK((*result)[0].uri.toString() == expectedUri);
}

/**
 * @brief Asserts that Go-To-Definition on a token in a document resolves to expected target URI.
 * @param[in] baseReq Base definition request.
 * @param[in] token Target token text.
 * @param[in] searchPos Byte search offset.
 * @param[in] targetUri Expected resolved target URI.
 */
void AssertDefinitionAtToken(const features::DefinitionRequest& baseReq, const std::string& token, size_t searchPos,
                             const std::string& targetUri)
{
    const SourcePos pos = FindPos(baseReq.sourceCode, token, searchPos);
    features::DefinitionRequest req = baseReq;
    req.position = lsp::Position{pos.line, pos.character};
    AssertDefinitionTarget(req, targetUri);
}

/**
 * @brief Test fixture holding randomized symbol names for multi-file workspace parity testing.
 */
struct WorkspaceFixture
{
    std::string nsRoot;
    std::string nsSub;
    std::string enumType;
    std::string enumVersion;
    std::string parserNs;
    std::string keyPairClass;
    std::string deserializerBase;
    std::string deserializerDerived;
    std::string fnCheck;
    std::string fnGetVersion;
    std::string fnProcess;

    /**
     * @brief Generates a randomized workspace fixture.
     * @return Configured fixture instance.
     */
    static WorkspaceFixture CreateRandom()
    {
        return {
            .nsRoot = GenerateRandomSymbolName("meta_api"),
            .nsSub = GenerateRandomSymbolName("json"),
            .enumType = GenerateRandomSymbolName("Type"),
            .enumVersion = GenerateRandomSymbolName("Version"),
            .parserNs = GenerateRandomSymbolName("parser"),
            .keyPairClass = GenerateRandomSymbolName("KeyValuePair"),
            .deserializerBase = GenerateRandomSymbolName("Deserializer"),
            .deserializerDerived = GenerateRandomSymbolName("__Deserializer__"),
            .fnCheck = GenerateRandomSymbolName("CheckType"),
            .fnGetVersion = GenerateRandomSymbolName("GetVersion"),
            .fnProcess = GenerateRandomSymbolName("Process"),
        };
    }

    /**
     * @brief Builds json.as script source text.
     * @return Source code string.
     */
    std::string BuildJsonCode() const
    {
        return "namespace " + nsRoot +
               " {\n"
               "    namespace " +
               nsSub +
               " {\n"
               "        enum " +
               enumType +
               " { Object, Array };\n"
               "        enum " +
               enumVersion +
               " { V1, V2 };\n"
               "        namespace " +
               parserNs +
               " {\n"
               "            class " +
               keyPairClass +
               " {}\n"
               "            class " +
               deserializerBase +
               " {}\n"
               "        }\n"
               "        #if METAMOD_PLUGIN_ASLP\n"
               "        bool __METAMOD__ = true;\n"
               "        #endif\n"
               "    }\n"
               "}\n";
    }

    /**
     * @brief Builds v1.as script source text.
     * @return Source code string.
     */
    std::string BuildV1Code() const
    {
        return "#include \"json.as\"\n"
               "namespace " +
               nsRoot +
               " {\n"
               "    namespace " +
               nsSub +
               " {\n"
               "        namespace v1 {\n"
               "            class " +
               deserializerDerived + " : " + parserNs + "::" + deserializerBase +
               " {}\n"
               "            const " +
               nsRoot + "::" + nsSub + "::" + enumVersion + " " + fnGetVersion +
               "() {\n"
               "                return " +
               nsRoot + "::" + nsSub + "::" + enumVersion +
               "::V1;\n"
               "            }\n"
               "            bool " +
               fnCheck +
               "(int type) {\n"
               "                switch(type) {\n"
               "                    case 1: return true;\n"
               "                    default: return false;\n"
               "                }\n"
               "            }\n"
               "            void " +
               fnProcess + "(" + nsRoot + "::" + nsSub + "::" + parserNs + "::" + keyPairClass +
               "@ pair) {\n"
               "                " +
               nsRoot + "::" + nsSub + "::" + enumType + " t = " + nsRoot + "::" + nsSub + "::" + enumType +
               "::Object;\n"
               "                " +
               deserializerDerived +
               " Deserializer();\n"
               "            }\n"
               "        }\n"
               "    }\n"
               "}\n";
    }
};
} // namespace

TEST_SUITE_BEGIN("JsonParityInclude");

TEST_CASE("Extensionless include path resolution with directory traversal")
{
    const std::string subDirName = GenerateRandomSymbolName("sub");
    const std::string incDirName = GenerateRandomSymbolName("inc");
    const std::string baseFileName = GenerateRandomSymbolName("json");

    const auto tempRoot = std::filesystem::temp_directory_path() / GenerateRandomSymbolName("lsp_test_inc");
    const auto incDir = tempRoot / incDirName;
    const auto subDir = tempRoot / subDirName;
    std::filesystem::create_directories(incDir);
    std::filesystem::create_directories(subDir);

    const auto targetFilePath = incDir / (baseFileName + ".as");
    {
        std::ofstream out(targetFilePath);
        out << "// target script\n";
    }

    const auto sourceFilePath = subDir / "caller.as";
    const std::string sourceFilePathStr = sourceFilePath.string();
    const std::string rawIncludePath = "../" + incDirName + "/" + baseFileName;

    const std::vector<std::string> searchDirs = {tempRoot.string()};
    utils::IncludeResolveRequest req;
    req.includePath = rawIncludePath;
    req.currentFilePath = sourceFilePathStr;
    req.searchDirectories = searchDirs;
    req.implicitExtension = ".as";

    const std::string resolved = utils::IncludeResolver::ResolveIncludePath(req);
    CHECK_FALSE(resolved.empty());
    CHECK(std::filesystem::equivalent(std::filesystem::path(resolved), targetFilePath));

    std::filesystem::remove_all(tempRoot);
}

TEST_CASE("Implicit default constructor synthesized for class with no explicit constructors")
{
    const std::string baseClass = GenerateRandomSymbolName("BaseDeserializer");
    const std::string derivedClass = GenerateRandomSymbolName("CustomDeserializer");
    const std::string varName = GenerateRandomSymbolName("d");

    const std::string code = "abstract class " + baseClass + " { void Init() {} }\n" + "class " + derivedClass + " : " +
                             baseClass + " {}\n" + "void Test() {\n" + "    " + derivedClass + " " + varName + "();\n" +
                             "}\n";

    const auto diags = AnalyzeSnippet(code);
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-matching-constructor"));
}

TEST_CASE("CFG switch with default clause and all returning branches satisfies return check")
{
    const std::string fnName = GenerateRandomSymbolName("CheckType");

    const std::string code = "bool " + fnName + "(int type) {\n" + "    switch(type) {\n" +
                             "        case 1: return true;\n" + "        default: return false;\n" + "    }\n" + "}\n";

    const auto diags = AnalyzeSnippet(code);
    CHECK_FALSE(HasDiagCode(diags, "as-err-not-all-paths-return"));
}

TEST_CASE("Scoped enum types and enumerators resolve cleanly across namespaces")
{
    const std::string nsRoot = GenerateRandomSymbolName("meta_api");
    const std::string nsSub = GenerateRandomSymbolName("json");
    const std::string enumType = GenerateRandomSymbolName("Type");
    const std::string enumVersion = GenerateRandomSymbolName("Version");
    const std::string parserNs = GenerateRandomSymbolName("parser");
    const std::string payloadClass = GenerateRandomSymbolName("KeyValuePair");

    const std::string code =
        "namespace " + nsRoot + " {\n" + "    namespace " + nsSub + " {\n" + "        enum " + enumType +
        " { Undefined = 0, Object, Array };\n" + "        enum " + enumVersion + " { V1 = 1, V2 = 2 };\n" +
        "        namespace " + parserNs + " {\n" + "            class " + payloadClass + " { string key; }\n" +
        "        }\n" + "    }\n" + "}\n" + "namespace " + nsRoot + " {\n" + "    namespace " + nsSub + " {\n" +
        "        namespace v1 {\n" + "            const " + nsRoot + "::" + nsSub + "::" + enumVersion +
        " GetVersion() {\n" + "                return " + nsRoot + "::" + nsSub + "::" + enumVersion + "::V1;\n" +
        "            }\n" + "            void Process(" + nsRoot + "::" + nsSub + "::" + parserNs +
        "::" + payloadClass + "@ pair) {\n" + "                " + nsRoot + "::" + nsSub + "::" + enumType +
        " t = " + nsRoot + "::" + nsSub + "::" + enumType + "::Object;\n" + "            }\n" + "        }\n" +
        "    }\n" + "}\n";

    const auto diags = AnalyzeSnippet(code);
    CHECK_FALSE(HasDiagCode(diags, "as-err-unresolved-type"));
    CHECK_FALSE(HasDiagCode(diags, "as-warn-undeclared-identifier"));
}

TEST_CASE("Go-To-Definition prioritizes workspace symbol over predefined stub")
{
    const std::string typeName = GenerateRandomSymbolName("WidgetType");
    const std::string nsName = GenerateRandomSymbolName("WorkspaceNs");

    parser::AngelScriptParser parser;
    SymbolTable table;

    // Add stub to predefined file
    Symbol stub;
    stub.name = typeName;
    stub.type = SymbolType::Class;
    stub.fileUri = "file:///workspace/as.predefined";
    stub.signature = ClassSignature{};
    table.AddSymbol(stub);

    // Add workspace declaration in workspace file
    Symbol wsSym;
    wsSym.name = nsName + "::" + typeName;
    wsSym.type = SymbolType::Class;
    wsSym.fileUri = "file:///workspace/models.as";
    wsSym.signature = ClassSignature{};
    table.AddSymbol(wsSym);

    const std::string script = "void Run() { " + nsName + "::" + typeName + " w; }";
    TSTree* tree = parser.Parse(script);
    REQUIRE(tree != nullptr);

    ScopeIndex scopeIndex;
    // Find position of typeName inside script
    const size_t pos = script.find(typeName);
    REQUIRE(pos != std::string::npos);

    features::DefinitionRequest req{
        .uri = "file:///workspace/main.as",
        .sourceCode = script,
        .tree = tree,
        .symbolTable = table,
        .scopeIndex = scopeIndex,
        .position = lsp::Position{0u, static_cast<lsp::uint>(pos + 2)},
        .resolveInclude = {},
        .predefinedExtension = ".as.predefined",
    };

    const auto result = features::GetDefinition(req);
    REQUIRE(result.has_value());
    REQUIRE(!result->empty());

    // Verify resolved location points to workspace file, NOT predefined file
    CHECK((*result)[0].uri.toString() == "file:///workspace/models.as");

    ts_tree_delete(tree);
}

TEST_CASE("Multi-file workspace parity: json.as and v1.as with scoped definitions")
{
    const auto f = WorkspaceFixture::CreateRandom();
    const std::string jsonUri = "file:///scripts/json.as";
    const std::string jsonCode = f.BuildJsonCode();
    const std::string v1Uri = "file:///scripts/v1.as";
    const std::string v1Code = f.BuildV1Code();

    parser::AngelScriptParser parser;
    SymbolTable sharedTable;
    TSTree* jsonTree = nullptr;
    TSTree* v1Tree = nullptr;

    const auto jsonDiags = AnalyzeWorkspaceDoc(sharedTable, {jsonUri, jsonCode}, parser, jsonTree);
    const auto v1Diags = AnalyzeWorkspaceDoc(sharedTable, {v1Uri, v1Code}, parser, v1Tree);

    CHECK_FALSE(HasDiagCode(jsonDiags, "as-err-unresolved-type"));
    CHECK_FALSE(HasDiagCode(v1Diags, "as-err-unresolved-type"));
    CHECK_FALSE(HasDiagCode(v1Diags, "as-warn-undeclared-identifier"));
    CHECK_FALSE(HasDiagCode(v1Diags, "as-err-not-all-paths-return"));
    CHECK_FALSE(HasDiagCode(v1Diags, "as-err-no-matching-constructor"));

    ScopeIndex v1ScopeIndex;
    features::DefinitionRequest baseReq{v1Uri, v1Code, v1Tree, sharedTable, v1ScopeIndex, {}, {}, ".as.predefined"};

    const size_t typePos = v1Code.find(f.enumType + "::Object");
    REQUIRE(typePos != std::string::npos);
    AssertDefinitionAtToken(baseReq, f.enumType, typePos, jsonUri);

    const size_t v1EnumPos = v1Code.find(f.enumVersion + "::V1");
    REQUIRE(v1EnumPos != std::string::npos);
    AssertDefinitionAtToken(baseReq, "V1", v1EnumPos, jsonUri);

    if (jsonTree)
        ts_tree_delete(jsonTree);
    if (v1Tree)
        ts_tree_delete(v1Tree);
}

TEST_SUITE_END();
