#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "config/EngineRuleConfig.h"
#include "config/EngineRuleParser.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::diagnostics;
using namespace angel_lsp::parser;

namespace
{
std::vector<Diagnostic> AnalyzeSnippet(const std::string& code, const angel_lsp::config::EngineRuleConfig* rules,
                                       const std::string& fileUri = "file:///no_handle_types.as")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    auto diags = collector.CollectSymbols(fileUri, code, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    TSTree* tree = parser.Parse(code);
    NodeIndex nodeIndex(ts_tree_root_node(tree));
    request.nodeIndex = &nodeIndex;
    request.engineRules = rules;

    SemanticAnalyzer analyzer(nullptr);
    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), std::make_move_iterator(semDiags.begin()), std::make_move_iterator(semDiags.end()));

    if (tree)
    {
        ts_tree_delete(tree);
    }
    return diags;
}

size_t CountCode(const std::vector<Diagnostic>& diagnostics, std::string_view code)
{
    return static_cast<size_t>(
        std::count_if(diagnostics.begin(), diagnostics.end(), [code](const Diagnostic& d) { return d.code == code; }));
}
} // namespace

TEST_SUITE("analysis/rules/NoHandleTypes")
{
    TEST_CASE("NoHandleTypes - Key absent produces no diagnostic")
    {
        std::mt19937_64 rng(0x4011A001);
        const std::string valType = angel_lsp::test::GenerateIdentifier(rng, "Val");
        const std::string holderCls = angel_lsp::test::GenerateIdentifier(rng, "Holder");
        const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Run");

        const std::string code = "class " + valType + " {}\n" + valType + "@ g_global;\n" + valType +
                                 "@[] g_arr;\narray<" + valType + "@> g_tmpl;\nclass " + holderCls + " { " + valType +
                                 "@ m_field; }\n" + valType + "@ " + fnName + "(" + valType + "@ p)\n{\n    " +
                                 valType + "@ localH;\n    " + valType + " val;\n    @" +
                                 "val;\n    return localH;\n}\n";

        CHECK(CountCode(AnalyzeSnippet(code, nullptr), codes::HandleOnNoHandleType) == 0);

        angel_lsp::config::EngineRuleConfig emptyRules;
        CHECK(CountCode(AnalyzeSnippet(code, &emptyRules), codes::HandleOnNoHandleType) == 0);
    }

    TEST_CASE("NoHandleTypes - Flags direct handles in local, global, member, param, return, and funcdef")
    {
        std::mt19937_64 rng(0x4011A002);
        const std::string valType = angel_lsp::test::GenerateIdentifier(rng, "Val");
        const std::string holderCls = angel_lsp::test::GenerateIdentifier(rng, "Holder");
        const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Use");
        const std::string fdName = angel_lsp::test::GenerateIdentifier(rng, "Cb");

        angel_lsp::config::EngineRuleConfig rules;
        rules.noHandleTypes.insert(valType);

        const std::string localCode = "class " + valType + " {}\nvoid " + fnName + "() { " + valType + "@ localH; }\n";
        CHECK(CountCode(AnalyzeSnippet(localCode, &rules), codes::HandleOnNoHandleType) == 1);

        const std::string globalCode = "class " + valType + " {}\n" + valType + "@ g_val;\n";
        CHECK(CountCode(AnalyzeSnippet(globalCode, &rules), codes::HandleOnNoHandleType) == 1);

        const std::string memberCode =
            "class " + valType + " {}\nclass " + holderCls + " { " + valType + "@ m_val; }\n";
        CHECK(CountCode(AnalyzeSnippet(memberCode, &rules), codes::HandleOnNoHandleType) == 1);

        const std::string paramCode = "class " + valType + " {}\nvoid " + fnName + "(" + valType + "@ p) {}\n";
        CHECK(CountCode(AnalyzeSnippet(paramCode, &rules), codes::HandleOnNoHandleType) == 1);

        const std::string returnCode = "class " + valType + " {}\n" + valType + "@ " + fnName + "() { return null; }\n";
        CHECK(CountCode(AnalyzeSnippet(returnCode, &rules), codes::HandleOnNoHandleType) == 1);

        const std::string funcdefCode =
            "class " + valType + " {}\nfuncdef " + valType + "@ " + fdName + "(" + valType + "@ p);\n";
        CHECK(CountCode(AnalyzeSnippet(funcdefCode, &rules), codes::HandleOnNoHandleType) == 2);
    }

    TEST_CASE("NoHandleTypes - Flags T@[], array<T@>, nested arrays, and @x")
    {
        std::mt19937_64 rng(0x4011A003);
        const std::string valType = angel_lsp::test::GenerateIdentifier(rng, "Val");
        const std::string holderCls = angel_lsp::test::GenerateIdentifier(rng, "Box");
        const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Proc");

        angel_lsp::config::EngineRuleConfig rules;
        rules.noHandleTypes.insert(valType);

        // Local, global, member, param, return for T@[] and array<T@>
        const std::string arrayBracketCode = "class " + valType + " {}\n" + valType + "@[] g_arr;\nclass " + holderCls +
                                             " { " + valType + "@[] m_arr; }\n" + valType + "@[] " + fnName + "(" +
                                             valType + "@[] p)\n{\n    " + valType +
                                             "@[] l_arr;\n    return l_arr;\n}\n";
        CHECK(CountCode(AnalyzeSnippet(arrayBracketCode, &rules), codes::HandleOnNoHandleType) == 5);

        const std::string templateCode = "class array<T> {}\nclass " + valType + " {}\narray<" + valType +
                                         "@> g_arr;\nclass " + holderCls + " { array<" + valType +
                                         "@> m_arr; }\narray<" + valType + "@> " + fnName + "(array<" + valType +
                                         "@> p)\n{\n    array<" + valType + "@> l_arr;\n    array<array<" + valType +
                                         "@>> l_nested;\n    return l_arr;\n}\n";
        CHECK(CountCode(AnalyzeSnippet(templateCode, &rules), codes::HandleOnNoHandleType) == 6);

        // Unary handle-of expression @x
        const std::string varName = angel_lsp::test::GenerateIdentifier(rng, "inst");
        const std::string addrOfCode = "class " + valType + " {}\nvoid " + fnName + "()\n{\n    " + valType + " " +
                                       varName + ";\n    @" + varName + ";\n}\n";
        CHECK(CountCode(AnalyzeSnippet(addrOfCode, &rules), codes::HandleOnNoHandleType) == 1);
    }

    TEST_CASE("NoHandleTypes - Valid array handles and unrelated classes are allowed")
    {
        std::mt19937_64 rng(0x4011A004);
        const std::string valType = angel_lsp::test::GenerateIdentifier(rng, "Val");
        const std::string refType = angel_lsp::test::GenerateIdentifier(rng, "RefObj");
        const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Ok");

        angel_lsp::config::EngineRuleConfig rules;
        rules.noHandleTypes.insert(valType);

        const std::string code = "class array<T> {}\nclass " + valType + " {}\nclass " + refType + " {}\n" + valType +
                                 "[]@ g_arrHandle;\narray<" + valType + ">@ g_tmplHandle;\n" + refType +
                                 "@ g_ref;\nvoid " + fnName + "(const " + valType + "[]@ &in p, array<" + valType +
                                 "[]@> nested)\n{\n    " + valType + "[]@[]@ multi;\n    array<" + valType +
                                 "> arr;\n    @arr;\n    " + refType + " r;\n    @" + "r;\n}\n";
        CHECK(CountCode(AnalyzeSnippet(code, &rules), codes::HandleOnNoHandleType) == 0);
    }

    TEST_CASE("NoHandleTypes - Resolves typedef aliases and E261 suppression alias")
    {
        std::mt19937_64 rng(0x4011A005);
        const std::string valType = angel_lsp::test::GenerateIdentifier(rng, "Val");
        const std::string aliasType = angel_lsp::test::GenerateIdentifier(rng, "ValAlias");
        const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Check");

        angel_lsp::config::EngineRuleConfig rules;
        rules.noHandleTypes.insert(valType);

        const std::string code = "class " + valType + " {}\ntypedef " + valType + " " + aliasType + ";\n" + aliasType +
                                 "@ g_alias;\nvoid " + fnName + "()\n{\n    " + aliasType + " x;\n    @x;\n}\n";
        CHECK(CountCode(AnalyzeSnippet(code, &rules), codes::HandleOnNoHandleType) == 2);
        CHECK(GetDiagnosticAlias(codes::HandleOnNoHandleType) == "E261");
        CHECK(GetCanonicalDiagnosticCode("E261") == codes::HandleOnNoHandleType);
    }

    TEST_CASE("NoHandleTypes - Flags string@ with sdk-addons rules fixture")
    {
        const std::filesystem::path rulesPath =
            std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / "sdk-addons.angelscript.rules.json";
        const auto rules = angel_lsp::config::ParseEngineRuleConfigFile(rulesPath);
        REQUIRE(rules.has_value());

        const std::string code = "class string {}\n"
                                 "void main() {\n"
                                 "    string@ s;\n"
                                 "}\n";
        const auto diags = AnalyzeSnippet(code, &*rules);
        CHECK(CountCode(diags, codes::HandleOnNoHandleType) == 1);
    }
}
