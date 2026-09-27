#include <doctest/doctest.h>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/TypeConversionChecker.h"
#include "config/ServerConfig.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;
using namespace angel_lsp::config;
using angel_lsp::test::GenerateRandomSymbolName;

namespace
{
struct PipelineResult
{
    std::vector<Diagnostic> diagnostics;
};

PipelineResult RunPipeline(const std::string& sourceCode, const EngineProperties* engineProps = nullptr)
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n;

    AngelScriptParser symbolParser;
    SymbolCollector symbolCollector(nullptr);
    symbolCollector.CollectSymbols("file:///test.as", sourceCode, symbolParser, table);

    AngelScriptParser scopeParser;
    LocalScopeCollector scopeCollector(nullptr);

    SemanticAnalysisRequest req{table, "file:///test.as", "", &i18n};
    req.engineProperties = engineProps;
    req.scopeRoot = scopeCollector.CollectScopes(sourceCode, scopeParser);

    AngelScriptParser treeParser;
    req.sourceCode = sourceCode;
    req.tree = treeParser.Parse(sourceCode);

    SemanticAnalyzer analyzer(nullptr);
    PipelineResult res;
    res.diagnostics = analyzer.Analyze(req);

    TypeConversionCheckRequest tcReq;
    tcReq.root = ts_tree_root_node(req.tree);
    tcReq.sourceCode = sourceCode;
    tcReq.scopeRoot = req.scopeRoot.get();

    DiagnosticContext tcCtx(req, res.diagnostics);
    CheckTypeConversions(tcReq, tcCtx);

    return res;
}

bool HasDiagnostic(const std::vector<Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(),
                       [code](const Diagnostic& d) { return d.code == code; });
}

struct ArgvHelper
{
    std::vector<std::string> storage;
    std::vector<char*> argv;

    ArgvHelper(std::initializer_list<std::string> args) : storage(args)
    {
        for (auto& s : storage)
        {
            argv.push_back(s.data());
        }
    }

    int argc() const
    {
        return static_cast<int>(argv.size());
    }

    char** data()
    {
        return argv.data();
    }
};
} // namespace

TEST_CASE("EngineProperties: asEP_REQUIRE_ENUM_SCOPE on/off behavior")
{
    const std::string enumName = GenerateRandomSymbolName("Color");
    const std::string memRed = GenerateRandomSymbolName("Red");
    const std::string memBlue = GenerateRandomSymbolName("Blue");
    const std::string fnName = GenerateRandomSymbolName("TestFunc");
    const std::string varLocal = GenerateRandomSymbolName("cLocal");
    const std::string varGlobal = GenerateRandomSymbolName("g_Color");

    SUBCASE("Default off: unqualified enumerators resolve cleanly")
    {
        const std::string code = "enum " + enumName + " { " + memRed + ", " + memBlue + " };\n" +
                                 enumName + " " + varGlobal + " = " + memRed + ";\n" +
                                 "void " + fnName + "() {\n" +
                                 "    " + enumName + " " + varLocal + " = " + memBlue + ";\n" +
                                 "}\n";

        EngineProperties engine;
        engine.requireEnumScope = false;
        const auto res = RunPipeline(code, &engine);
        CHECK_FALSE(HasDiagnostic(res.diagnostics, "as-err-enum-scope-required"));
    }

    SUBCASE("Enabled on: unqualified enumerators emit as-err-enum-scope-required")
    {
        const std::string code = "enum " + enumName + " { " + memRed + ", " + memBlue + " };\n" +
                                 enumName + " " + varGlobal + " = " + memRed + ";\n" +
                                 "void " + fnName + "() {\n" +
                                 "    " + enumName + " " + varLocal + " = " + memBlue + ";\n" +
                                 "}\n";

        EngineProperties engine;
        engine.requireEnumScope = true;
        const auto res = RunPipeline(code, &engine);
        CHECK(HasDiagnostic(res.diagnostics, "as-err-enum-scope-required"));
    }

    SUBCASE("Enabled on: scoped enumerators Enum::Member resolve without error")
    {
        const std::string code = "enum " + enumName + " { " + memRed + ", " + memBlue + " };\n" +
                                 enumName + " " + varGlobal + " = " + enumName + "::" + memRed + ";\n" +
                                 "void " + fnName + "() {\n" +
                                 "    " + enumName + " " + varLocal + " = " + enumName + "::" + memBlue + ";\n" +
                                 "}\n";

        EngineProperties engine;
        engine.requireEnumScope = true;
        const auto res = RunPipeline(code, &engine);
        CHECK_FALSE(HasDiagnostic(res.diagnostics, "as-err-enum-scope-required"));
    }
}

TEST_CASE("EngineProperties: asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT on/off behavior")
{
    const std::string className = GenerateRandomSymbolName("CustomType");
    const std::string varName = GenerateRandomSymbolName("inst");
    const std::string fnName = GenerateRandomSymbolName("CreateInstance");

    const std::string code = "class " + className + " {\n" +
                             "    " + className + "(int a) {}\n" +
                             "};\n" +
                             "void " + fnName + "() {\n" +
                             "    " + className + " " + varName + ";\n" +
                             "}\n";

    SUBCASE("Default off: class with only non-default ctor cannot default construct")
    {
        EngineProperties engine;
        engine.alwaysImplDefaultConstruct = false;
        const auto res = RunPipeline(code, &engine);
        CHECK(HasDiagnostic(res.diagnostics, "as-err-no-default-constructor"));
    }

    SUBCASE("Enabled on: implicit default constructor is permitted")
    {
        EngineProperties engine;
        engine.alwaysImplDefaultConstruct = true;
        const auto res = RunPipeline(code, &engine);
        CHECK_FALSE(HasDiagnostic(res.diagnostics, "as-err-no-default-constructor"));
    }
}

TEST_CASE("EngineProperties: asEP_IGNORE_DUPLICATE_SHARED_INTF on/off behavior")
{
    const std::string ifaceName = GenerateRandomSymbolName("IShared");
    const std::string methodName = GenerateRandomSymbolName("Execute");

    const std::string code = "shared interface " + ifaceName + " { void " + methodName + "(); }\n" +
                             "shared interface " + ifaceName + " { void " + methodName + "(); }\n";

    SUBCASE("Default off: duplicate shared interface reports duplicate symbol")
    {
        EngineProperties engine;
        engine.ignoreDuplicateSharedIntf = false;
        const auto res = RunPipeline(code, &engine);
        CHECK(HasDiagnostic(res.diagnostics, "as-err-duplicate-symbol"));
    }

    SUBCASE("Enabled on: duplicate shared interface is accepted cleanly")
    {
        EngineProperties engine;
        engine.ignoreDuplicateSharedIntf = true;
        const auto res = RunPipeline(code, &engine);
        CHECK_FALSE(HasDiagnostic(res.diagnostics, "as-err-duplicate-symbol"));
    }
}

TEST_CASE("EngineProperties: CLI flags and boolean value parsing")
{
    SUBCASE("Flag with boolean literals")
    {
        ArgvHelper args{"angel_lsp",
                        "--require-enum-scope=true",
                        "--always-impl-default-construct=1",
                        "--ignore-duplicate-shared-intf=on"};
        ServerConfig config = FromArgs(args.argc(), args.data());
        CHECK(config.engine.requireEnumScope == true);
        CHECK(config.engine.alwaysImplDefaultConstruct == true);
        CHECK(config.engine.ignoreDuplicateSharedIntf == true);
    }

    SUBCASE("Negative flags toggle off cleanly")
    {
        ArgvHelper args{"angel_lsp",
                        "--require-enum-scope=true",
                        "--no-require-enum-scope",
                        "--always-impl-default-construct=true",
                        "--no-always-impl-default-construct"};
        ServerConfig config = FromArgs(args.argc(), args.data());
        CHECK(config.engine.requireEnumScope == false);
        CHECK(config.engine.alwaysImplDefaultConstruct == false);
    }

    SUBCASE("Engine-property syntax with varied boolean forms")
    {
        ArgvHelper args{"angel_lsp",
                        "--engine-property=requireEnumScope=true",
                        "--engine-prop=alwaysImplDefaultConstruct=1",
                        "--engine-property=allowMultilineStrings=true",
                        "--engine-property=disallowValueAssignForRef=true"};
        ServerConfig config = FromArgs(args.argc(), args.data());
        CHECK(config.engine.requireEnumScope == true);
        CHECK(config.engine.alwaysImplDefaultConstruct == true);
        CHECK(config.engine.allowMultilineStrings == true);
        CHECK(config.engine.disallowValueAssignForRef == true);
    }
}
