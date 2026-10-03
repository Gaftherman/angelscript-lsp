/**
 * @file TypeConfigDecouplingAndAnonymousObjectsTest.cpp
 * @brief Invariant-based tests verifying full decoupling of configured types (stringTypeName, arrayTypeName)
 *        across inlay hints, code action interface stubs, and anonymous objects (typed initializer lists).
 */

#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolCollector.h"
#include "analysis/TypeSanitization.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "config/ServerConfig.h"
#include "features/code_action/CodeActionHandler.h"
#include "features/inlay_hint/InlayHintHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::features;
using namespace angel_lsp::parser;
using namespace angel_lsp::test;

namespace
{

/**
 * @brief Extracts text representation of an inlay hint label.
 * @param[in] hint Inlay hint object.
 * @return Concatenated label string.
 */
std::string ExtractHintLabel(const lsp::InlayHint& hint)
{
    if (std::holds_alternative<std::string>(hint.label))
    {
        return std::get<std::string>(hint.label);
    }
    if (std::holds_alternative<std::vector<lsp::InlayHintLabelPart>>(hint.label))
    {
        std::string res;
        for (const auto& part : std::get<std::vector<lsp::InlayHintLabelPart>>(hint.label))
        {
            res += part.value;
        }
        return res;
    }
    return "";
}

} // namespace

TEST_SUITE("TypeConfig_Decoupling")
{
    TEST_CASE("Decoupled String Config: string literals and type deduction use configured name")
    {
        const std::string customStr = GenerateRandomSymbolName("CustomStr");
        const std::string docUri = "file:///workspace/test_decoupled_str.as";

        config::ServerConfig cfg;
        cfg.types.stringTypeName = customStr;

        const std::string script = "class " + customStr +
                                   " {}\n"
                                   "void Main() {\n"
                                   "    " +
                                   customStr +
                                   " validStr;\n"
                                   "    string invalidStr;\n"
                                   "}\n";

        auto doc = CreateTestDocumentWithConfig(docUri, script, cfg);
        REQUIRE(doc.get() != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        bool foundUnresolvedString = false;
        for (const auto& diag : diagnostics)
        {
            if ((diag.code == "as-err-unresolved-type" || diag.code == "E_UNKNOWN_TYPE") && diag.range.start.line == 3)
            {
                foundUnresolvedString = true;
            }
        }
        CHECK(foundUnresolvedString);
    }

    TEST_CASE("Decoupled Array Config: bracket desugaring and custom container isolation")
    {
        const std::string customArray = GenerateRandomSymbolName("CustomVector");
        const std::string elemType = GenerateRandomSymbolName("DataItem");

        // 1. Single dimension desugaring
        const std::string singleDesugar = DesugarArrayBrackets(elemType + "[]", customArray);
        CHECK(singleDesugar == customArray + "<" + elemType + ">");

        // 2. Multidimensional desugaring
        const std::string multiDesugar = DesugarArrayBrackets(elemType + "[][]", customArray);
        CHECK(multiDesugar == customArray + "<" + customArray + "<" + elemType + ">>");

        // 3. Canonicalization respects configured container
        const std::string canonical = CanonicalizeArrayType(elemType + "[]", customArray);
        CHECK(canonical == customArray + "<" + elemType + ">");
    }

    TEST_CASE("Decoupled CodeAction QuickFix: Stubs respect configured stringTypeName")
    {
        const std::string customStr = GenerateRandomSymbolName("MyText");
        const std::string ifaceName = GenerateRandomSymbolName("IEntity");
        const std::string clsName = GenerateRandomSymbolName("Entity");
        const std::string docUri = "file:///workspace/test_codeaction_stub.as";

        config::ServerConfig cfg;
        cfg.types.stringTypeName = customStr;

        const std::string script = "interface " + ifaceName +
                                   " {\n"
                                   "    " +
                                   customStr +
                                   " GetName();\n"
                                   "    void SetName(" +
                                   customStr +
                                   " name);\n"
                                   "}\n"
                                   "class " +
                                   clsName + " : " + ifaceName +
                                   " {\n"
                                   "}\n";

        AngelScriptParser parser;
        TSTree* tree = parser.Parse(script);
        REQUIRE(tree != nullptr);

        SymbolTable symbolTable;
        SymbolCollector collector{nullptr};
        collector.CollectSymbols(docUri, script, parser, symbolTable);

        LocalScopeCollector scopeCollector{nullptr};
        ScopeIndex scopeIndex;
        auto rootScope = scopeCollector.CollectScopes(script, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(docUri, std::move(rootScope));
        }

        CodeActionRequest req{.uri = docUri,
                              .sourceCode = script,
                              .tree = tree,
                              .range = lsp::Range{{4, 0}, {5, 1}},
                              .context = lsp::CodeActionContext{},
                              .symbolTable = symbolTable,
                              .scopeIndex = scopeIndex,
                              .allowedRoots = {},
                              .config = &cfg};

        auto actions = GetCodeActions(req);
        REQUIRE(actions.has_value());

        bool foundStubFix = false;
        for (const auto& action : *actions)
        {
            if (action.title.find("Implement missing interface methods") != std::string::npos)
            {
                foundStubFix = true;
                REQUIRE(action.edit.has_value());
                REQUIRE(action.edit->changes.has_value());
                const auto& changes = action.edit->changes.value();
                const auto& edits = changes.at(lsp::DocumentUri::parse(docUri));
                REQUIRE(!edits.empty());

                // Assert that the generated stub uses "" for the configured string type
                CHECK(edits[0].newText.find(customStr + " GetName()") != std::string::npos);
                CHECK(edits[0].newText.find("return \"\";") != std::string::npos);
                CHECK(edits[0].newText.find("void SetName(" + customStr + " name)") != std::string::npos);
            }
        }
        CHECK(foundStubFix);

        ts_tree_delete(tree);
    }

    TEST_CASE("Decoupled Inlay Hints: Binary expression wider type uses configured stringTypeName")
    {
        const std::string customStr = GenerateRandomSymbolName("CustomString");
        const std::string docUri = "file:///workspace/test_inlay_binary.as";

        config::ServerConfig cfg;
        cfg.types.stringTypeName = customStr;

        const std::string script = "class " + customStr +
                                   " {}\n"
                                   "void TestBinary() {\n"
                                   "    auto res = \"left\" + \"right\";\n"
                                   "}\n";

        AngelScriptParser parser;
        TSTree* tree = parser.Parse(script);
        REQUIRE(tree != nullptr);

        SymbolTable symbolTable;
        SymbolCollector collector{nullptr};
        collector.CollectSymbols(docUri, script, parser, symbolTable);

        LocalScopeCollector scopeCollector{nullptr};
        ScopeIndex scopeIndex;
        auto rootScope = scopeCollector.CollectScopes(script, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(docUri, std::move(rootScope));
        }

        InlayHintRequest req{.uri = docUri,
                             .sourceCode = script,
                             .tree = tree,
                             .range = lsp::Range{{0, 0}, {4, 0}},
                             .symbolTable = symbolTable,
                             .scopeIndex = scopeIndex,
                             .suppressWhenArgumentMatchesName = false,
                             .logger = nullptr,
                             .maxParameters = 0,
                             .maxLength = 0,
                             .omittedDefaultArguments = config::OmittedDefaultArgumentsMode::NameAndValue,
                             .config = &cfg};

        auto hints = GetInlayHints(req);
        REQUIRE(hints.has_value());

        bool foundTypeHint = false;
        for (const auto& hint : *hints)
        {
            const std::string label = ExtractHintLabel(hint);
            if (label.find(customStr) != std::string::npos)
            {
                foundTypeHint = true;
            }
        }
        CHECK(foundTypeHint);

        ts_tree_delete(tree);
    }
}

TEST_SUITE("AnonymousObjects_TypedInitializerLists")
{
    TEST_CASE("Typed Initializer List: expression type deduction for template and container types")
    {
        const std::string containerName = GenerateRandomSymbolName("ArrayContainer");
        const std::string fnName = GenerateRandomSymbolName("Consume");
        const std::string docUri = "file:///workspace/test_typed_init.as";

        const std::string script = "class " + containerName +
                                   "<T> {}\n"
                                   "void " +
                                   fnName + "(" + containerName +
                                   "<int> arg) {}\n"
                                   "void Main() {\n"
                                   "    " +
                                   fnName + "(" + containerName +
                                   "<int> = {1, 2, 3});\n"
                                   "}\n";

        AngelScriptParser parser;
        TSTree* tree = parser.Parse(script);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        TSNode typedInitNode{};
        TSTreeCursor cursor = ts_tree_cursor_new(root);
        bool hasNode = ts_tree_cursor_goto_first_child(&cursor);
        while (hasNode)
        {
            TSNode current = ts_tree_cursor_current_node(&cursor);
            if (std::string_view(ts_node_type(current)) == "typed_initializer_list")
            {
                typedInitNode = current;
                break;
            }
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                hasNode = true;
                continue;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                hasNode = true;
                continue;
            }
            hasNode = false;
            while (ts_tree_cursor_goto_parent(&cursor))
            {
                if (ts_tree_cursor_goto_next_sibling(&cursor))
                {
                    hasNode = true;
                    break;
                }
            }
        }
        ts_tree_cursor_delete(&cursor);

        REQUIRE(!ts_node_is_null(typedInitNode));

        SymbolTable symbolTable;
        SymbolCollector collector{nullptr};
        collector.CollectSymbols(docUri, script, parser, symbolTable);

        ExpressionTypeContext exprCtx{nullptr, symbolTable, script, docUri};
        std::string deduced = ResolveExpressionType(typedInitNode, exprCtx);

        // Expression type must resolve to the declared container with template arguments
        CHECK(deduced == containerName + "<int>");

        ts_tree_delete(tree);
    }

    TEST_CASE("Anonymous Objects: Nested typed initializer lists and weakref handle patterns")
    {
        const std::string dictName = GenerateRandomSymbolName("CustomDictionary");
        const std::string targetName = GenerateRandomSymbolName("EntityItem");
        const std::string weakrefName = GenerateRandomSymbolName("weakref");
        const std::string docUri = "file:///workspace/test_nested_anon.as";

        const std::string script = "class " + dictName +
                                   " {}\n"
                                   "class " +
                                   targetName +
                                   " {}\n"
                                   "class " +
                                   weakrefName +
                                   "<T> {}\n"
                                   "void Worker() {\n"
                                   "    " +
                                   dictName + " = {{1, " + dictName +
                                   " = {{2, 3}}}};\n"
                                   "    " +
                                   weakrefName + "<" + targetName +
                                   ">@ handle;\n"
                                   "}\n";

        auto doc = CreateTestDocument(docUri, script);
        REQUIRE(doc.get() != nullptr);

        // Nested typed initializer lists and weakref template handles must parse and analyze cleanly
        auto diagnostics = doc->GetDiagnostics();
        bool hasFatalErrors = false;
        for (const auto& diag : diagnostics)
        {
            if (diag.code == "as-err-unresolved-type" || diag.code == "E_UNKNOWN_TYPE")
            {
                hasFatalErrors = true;
            }
        }
        CHECK_FALSE(hasFatalErrors);
    }
}
