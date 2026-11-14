#include "analysis/index/InlayHintFacts.h"

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/InlayCalleeResolution.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "features/inlay_hint/InlayHintHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <ostream>
#include <string>

using namespace angel_lsp;
using namespace angel_lsp::analysis;

TEST_SUITE("analysis/index/InlayHintFacts")
{
    TEST_CASE("InlayHintFacts - Syntax Sites Extraction and Range Filtering")
    {
        parser::AngelScriptParser parser;

        const std::string fnName = test::GenerateRandomSymbolName("fn");
        const std::string varName = test::GenerateRandomSymbolName("autoVar");
        const std::string enumName = test::GenerateRandomSymbolName("Color");
        const std::string memberRed = test::GenerateRandomSymbolName("Red");
        const std::string memberGreen = test::GenerateRandomSymbolName("Green");
        const std::string memberBlue = test::GenerateRandomSymbolName("Blue");

        const std::string code = "enum " + enumName +
                                 " {\n"
                                 "    " +
                                 memberRed +
                                 ",\n"
                                 "    " +
                                 memberGreen +
                                 " = 10,\n"
                                 "    " +
                                 memberBlue +
                                 "\n"
                                 "};\n"
                                 "void TestFunc() {\n"
                                 "    auto " +
                                 varName +
                                 " = 42;\n"
                                 "    auto@ handleVar = " +
                                 varName +
                                 ";\n"
                                 "    " +
                                 fnName +
                                 "(100, namedArg: 200);\n"
                                 "    auto lambda = function(x, int y) { return x + y; };\n"
                                 "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        NodeIndex nodeIndex(root, NodeIndexOptions{.source = code});

        SUBCASE("Document-wide extraction captures all site categories")
        {
            InlayHintSites sites = CollectInlayHintSites(nodeIndex, 0, 15, code);

            // 1. Enum sites
            REQUIRE(sites.enumDeclarations.size() == 1);
            const auto& enumDecl = sites.enumDeclarations[0];
            CHECK(enumDecl.valueCount == 3);
            REQUIRE(sites.enumValues.size() >= 3);

            const auto& redVal = sites.enumValues[enumDecl.firstValue];
            CHECK(redVal.memberName == memberRed);
            CHECK_FALSE(redVal.hasExplicitValue);
            CHECK(redVal.implicitValue == 0);

            const auto& greenVal = sites.enumValues[enumDecl.firstValue + 1];
            CHECK(greenVal.memberName == memberGreen);
            CHECK(greenVal.hasExplicitValue);
            CHECK(greenVal.explicitValue == 10);
            CHECK(greenVal.implicitValue == 10);

            const auto& blueVal = sites.enumValues[enumDecl.firstValue + 2];
            CHECK(blueVal.memberName == memberBlue);
            CHECK_FALSE(blueVal.hasExplicitValue);
            CHECK(blueVal.implicitValue == 11);

            // 2. Auto variable declarators
            REQUIRE(sites.autoVars.size() == 3);
            CHECK(sites.autoVars[0].nameRange.startLine == 6);
            CHECK_FALSE(static_cast<bool>(sites.autoVars[0].isHandle));
            CHECK(sites.autoVars[1].nameRange.startLine == 7);
            CHECK(static_cast<bool>(sites.autoVars[1].isHandle));
            CHECK(sites.autoVars[2].nameRange.startLine == 9);
            CHECK_FALSE(static_cast<bool>(sites.autoVars[2].isHandle));

            // 3. Call expressions and arguments
            REQUIRE(sites.calls.size() >= 1);
            bool foundCall = false;
            for (const auto& call : sites.calls)
            {
                if (call.calleeText == fnName)
                {
                    foundCall = true;
                    CHECK(call.argumentCount == 2);
                    REQUIRE(sites.callArguments.size() >= call.firstArgument + 2);
                    const auto& arg0 = sites.callArguments[call.firstArgument];
                    const auto& arg1 = sites.callArguments[call.firstArgument + 1];
                    CHECK(arg0.text == "100");
                    CHECK(arg0.name.empty());
                    CHECK(arg1.text == "200");
                    CHECK(arg1.name == "namedArg");
                    break;
                }
            }
            CHECK(foundCall);

            // 4. Lambda sites and untyped parameters
            REQUIRE(sites.lambdas.size() == 1);
            const auto& lambda = sites.lambdas[0];
            CHECK(lambda.hasReturnStatement);
            CHECK_FALSE(lambda.hasExplicitReturnType);
            CHECK(lambda.paramCount == 1); // only 'x' is untyped, 'int y' has a written type
            REQUIRE(sites.lambdaParams.size() >= lambda.firstParam + 1);
            CHECK(sites.lambdaParams[lambda.firstParam].name == "x");
        }

        SUBCASE("Line range filtering restricts returned sites")
        {
            // Query lines 6 to 7 (auto variable declarations only)
            InlayHintSites sites = CollectInlayHintSites(nodeIndex, 6, 7, code);
            CHECK(sites.enumDeclarations.empty());
            CHECK(sites.enumValues.empty());
            CHECK(sites.autoVars.size() == 2);
            CHECK(sites.calls.empty());
            CHECK(sites.lambdas.empty());
        }

        SUBCASE("Enum value numbering parity: explicit preceding and following implicit members")
        {
            const std::string enumSeq = test::GenerateRandomSymbolName("EnumSeq");
            const std::string itemA = test::GenerateRandomSymbolName("ItemA");
            const std::string itemB = test::GenerateRandomSymbolName("ItemB");
            const std::string itemC = test::GenerateRandomSymbolName("ItemC");
            const std::string itemD = test::GenerateRandomSymbolName("ItemD");
            const std::string itemE = test::GenerateRandomSymbolName("ItemE");

            const std::string seqCode = "enum " + enumSeq +
                                        " {\n"
                                        "    " +
                                        itemA +
                                        ",\n"
                                        "    " +
                                        itemB +
                                        " = 100,\n"
                                        "    " +
                                        itemC +
                                        ",\n"
                                        "    " +
                                        itemD +
                                        " = 200,\n"
                                        "    " +
                                        itemE +
                                        "\n"
                                        "};\n";

            TSTree* seqTree = parser.Parse(seqCode);
            REQUIRE(seqTree != nullptr);
            TSNode seqRoot = ts_tree_root_node(seqTree);
            NodeIndex seqIndex(seqRoot, NodeIndexOptions{.source = seqCode});

            InlayHintSites seqSites = CollectInlayHintSites(seqIndex, 0, 10, seqCode);
            REQUIRE(seqSites.enumDeclarations.size() == 1);
            CHECK(seqSites.enumDeclarations[0].valueCount == 5);
            REQUIRE(seqSites.enumValues.size() == 5);

            CHECK(seqSites.enumValues[0].memberName == itemA);
            CHECK_FALSE(seqSites.enumValues[0].hasExplicitValue);
            CHECK(seqSites.enumValues[0].implicitValue == 0);

            CHECK(seqSites.enumValues[1].memberName == itemB);
            CHECK(seqSites.enumValues[1].hasExplicitValue);
            CHECK(seqSites.enumValues[1].explicitValue == 100);
            CHECK(seqSites.enumValues[1].implicitValue == 100);

            CHECK(seqSites.enumValues[2].memberName == itemC);
            CHECK_FALSE(seqSites.enumValues[2].hasExplicitValue);
            CHECK(seqSites.enumValues[2].implicitValue == 101);

            CHECK(seqSites.enumValues[3].memberName == itemD);
            CHECK(seqSites.enumValues[3].hasExplicitValue);
            CHECK(seqSites.enumValues[3].explicitValue == 200);
            CHECK(seqSites.enumValues[3].implicitValue == 200);

            CHECK(seqSites.enumValues[4].memberName == itemE);
            CHECK_FALSE(seqSites.enumValues[4].hasExplicitValue);
            CHECK(seqSites.enumValues[4].implicitValue == 201);

            ts_tree_delete(seqTree);
        }

        SUBCASE("Constructor init declarator extraction")
        {
            const std::string ctorCode = "class NetworkMessage {\n"
                                         "    NetworkMessage(int msg_type, int svc_message, int pEdict) {}\n"
                                         "}\n"
                                         "void main() {\n"
                                         "    int edict = 1;\n"
                                         "    NetworkMessage weapon(100, 200, edict);\n"
                                         "}\n";
            SymbolTable symTable;
            SymbolCollector symCollector{nullptr};
            symCollector.CollectSymbols("file:///test.as", ctorCode, parser, symTable);

            TSTree* ctorTree = parser.Parse(ctorCode);
            REQUIRE(ctorTree != nullptr);
            TSNode ctorRoot = ts_tree_root_node(ctorTree);
            NodeIndex ctorIndex(ctorRoot, NodeIndexOptions{.source = ctorCode});

            InlayHintSites ctorSites = CollectInlayHintSites(ctorIndex, 0, 10, ctorCode);
            CHECK(ctorSites.ctorInits.size() == 1);
            if (!ctorSites.ctorInits.empty())
            {
                CHECK(ctorSites.ctorInits[0].declaredTypeName == "NetworkMessage");
                CHECK(ctorSites.ctorInits[0].argumentCount == 3);
                ExpressionTypeContext ctx{nullptr, symTable, ctorCode, "file:///test.as"};
                ctx.nodeIndex = &ctorIndex;
                auto callee = ResolveConstructorInitCallee(ctorSites.ctorInits[0], ctorSites.callArguments, ctx);
                CHECK(callee.has_value());
                ScopeIndex scopeIndex;
                features::InlayHintRequest req{"file:///test.as",          ctorCode, &ctorIndex,
                                               lsp::Range{{0, 0}, {0, 0}}, symTable, scopeIndex};
                auto hints = features::GetInlayHints(req);
                CHECK(hints.has_value());
                if (hints.has_value())
                {
                    CHECK(hints->size() == 3);
                }
            }
            ts_tree_delete(ctorTree);
        }

        ts_tree_delete(tree);
    }
}
