#include "analysis/index/CallSiteFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE("analysis/index/CallSiteFacts")
{
    TEST_CASE("CallSiteFacts - Callee kinds and receiver operands")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string funcA = angel_lsp::test::GenerateRandomSymbolName("funcA");
        const std::string funcB = angel_lsp::test::GenerateRandomSymbolName("funcB");
        const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("NS");
        const std::string objName = angel_lsp::test::GenerateRandomSymbolName("obj");
        const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("method");

        const std::string code = "void " + fnName + "()\n{\n" + "    " + funcA + "();\n" + "    " + nsName +
                                 "::" + funcB + "();\n" + "    " + objName + "." + methodName + "();\n" + "    this." +
                                 methodName + "();\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        REQUIRE(facts.callSites.size() == 4);

        // 1. Unqualified identifier call
        const auto& call0 = facts.callSites[0];
        CHECK(call0.calleeKind == angel_lsp::analysis::CalleeKind::Identifier);
        CHECK(facts.TextOf(call0.calleeNameBytes) == funcA);
        CHECK(facts.TextOf(call0.calleeBytes) == funcA);
        CHECK(call0.isMemberCall == false);
        CHECK(call0.isThisCall == false);
        CHECK(call0.argumentCount == 0);
        CHECK(facts.FindCallSite(call0.bytes) == &call0);

        // 2. Scoped identifier call
        const auto& call1 = facts.callSites[1];
        CHECK(call1.calleeKind == angel_lsp::analysis::CalleeKind::Scoped);
        CHECK(facts.TextOf(call1.calleeNameBytes) == funcB);
        CHECK(angel_lsp::utils::ParentScopeView(facts.TextOf(call1.calleeBytes)) == nsName);
        CHECK(facts.TextOf(call1.calleeBytes) == (nsName + "::" + funcB));
        CHECK(call1.isMemberCall == false);

        // 3. Object member call
        const auto& call2 = facts.callSites[2];
        CHECK(call2.calleeKind == angel_lsp::analysis::CalleeKind::Member);
        CHECK(facts.TextOf(call2.calleeNameBytes) == methodName);
        CHECK(call2.isMemberCall == true);
        CHECK(call2.isThisCall == false);
        CHECK(facts.TextOf(call2.objectBytes) == objName);

        // 4. this member call
        const auto& call3 = facts.callSites[3];
        CHECK(call3.calleeKind == angel_lsp::analysis::CalleeKind::Member);
        CHECK(facts.TextOf(call3.calleeNameBytes) == methodName);
        CHECK(call3.isMemberCall == true);
        CHECK(call3.isThisCall == true);
        CHECK(facts.TextOf(call3.objectBytes) == "this");

        ts_tree_delete(tree);
    }

    TEST_CASE("CallSiteFacts - Arguments slicing, named arguments, and kind classification")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string targetFunc = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
        const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");
        const std::string varC = angel_lsp::test::GenerateRandomSymbolName("c");
        const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("param");

        const std::string code = "void " + fnName + "()\n{\n" + "    " + targetFunc + "(123, \"test\", null, " +
                                 paramName + ": " + varA + ", {1, 2}, " + "function(int x) { return x; }, " + varB +
                                 " ? " + varA + " : " + varC + ");\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        REQUIRE(facts.callSites.size() == 1);
        const auto& call = facts.callSites[0];
        CHECK(call.hasArgumentList == true);
        CHECK(call.argumentCount == 7);
        const auto args = facts.ArgumentsOf(call);
        REQUIRE(args.size() == 7);

        // Arg 0: Number literal
        const auto& arg0 = args[0];
        CHECK(arg0.kind == angel_lsp::analysis::CallArgumentKind::Literal);
        CHECK(facts.TextOf(arg0.bytes) == "123");
        CHECK(arg0.isNamed == false);
        CHECK(arg0.isNumericLiteral == true);

        // Arg 1: String literal
        const auto& arg1 = args[1];
        CHECK(arg1.kind == angel_lsp::analysis::CallArgumentKind::Literal);
        CHECK(facts.TextOf(arg1.bytes) == "\"test\"");

        // Arg 2: Null literal
        const auto& arg2 = args[2];
        CHECK(arg2.kind == angel_lsp::analysis::CallArgumentKind::Literal);
        CHECK(arg2.isNullLiteral == true);

        // Arg 3: Named argument
        const auto& arg3 = args[3];
        CHECK(arg3.isNamed == true);
        CHECK(facts.TextOf(arg3.nameBytes) == paramName);
        CHECK(facts.TextOf(arg3.bytes) == varA);
        CHECK(facts.TextOf(arg3.operandBytes) == varA);

        // Arg 4: Initializer list
        const auto& arg4 = args[4];
        CHECK(arg4.kind == angel_lsp::analysis::CallArgumentKind::InitializerList);

        // Arg 5: Lambda
        const auto& arg5 = args[5];
        CHECK(arg5.kind == angel_lsp::analysis::CallArgumentKind::Lambda);
        CHECK(arg5.hasLambdaParameterList == true);
        const auto lambdaParams = facts.LambdaParamsOf(arg5);
        REQUIRE(lambdaParams.size() == 1);
        CHECK(lambdaParams[0].hasWrittenType == true);
        CHECK(facts.TextOf(lambdaParams[0].typeBytes) == "int");

        // Arg 6: Ternary conditional expression
        const auto& arg6 = args[6];
        CHECK(arg6.kind == angel_lsp::analysis::CallArgumentKind::Ternary);
        const auto ternary = facts.TernaryOf(arg6);
        CHECK(facts.TextOf(ternary.consequenceBytes) == varA);
        CHECK(facts.TextOf(ternary.alternativeBytes) == varC);

        ts_tree_delete(tree);
    }

    TEST_CASE("CallSiteFacts - Argument ordering violation and delegate construction pattern")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string callBadOrder = angel_lsp::test::GenerateRandomSymbolName("badOrder");
        const std::string callDelegate = angel_lsp::test::GenerateRandomSymbolName("delegateCall");
        const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("p");

        const std::string code = "void " + fnName + "()\n{\n" + "    " + callBadOrder + "(" + paramName + ": 1, 2);\n" +
                                 "    " + callDelegate + "(this." + callDelegate + ");\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        REQUIRE(facts.callSites.size() == 2);

        // Call 0: Positional after named violation
        const auto& c0 = facts.callSites[0];
        CHECK(c0.hasNamedArguments == true);
        CHECK(c0.hasPositionalAfterNamed == true);
        CHECK(facts.arguments[c0.argumentBegin].isNamed == true);
        CHECK(facts.arguments[c0.argumentBegin + 1].isNamed == false);
        CHECK(facts.arguments[c0.argumentBegin + 1].isPositionalAfterNamed == true);

        // Call 1: Delegate construction pattern
        const auto& c1 = facts.callSites[1];
        CHECK(c1.isDelegateConstruction == true);
        CHECK(facts.arguments[c1.argumentBegin].isThisMemberAccess == true);
        CHECK(facts.ThisMemberName(facts.arguments[c1.argumentBegin]) == callDelegate);

        ts_tree_delete(tree);
    }

    TEST_CASE("CallSiteFacts - Construct call expression")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("CustomType");

        const std::string code =
            "void " + fnName + "()\n{\n" + "    array<int>(10);\n" + "    " + typeName + "<string>();\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        REQUIRE(facts.callSites.size() == 2);

        const auto& call0 = facts.callSites[0];
        CHECK(call0.isConstructCall == true);
        CHECK(call0.argumentCount == 1);
        CHECK(facts.TextOf(facts.ArgumentsOf(call0)[0].bytes) == "10");

        const auto& call1 = facts.callSites[1];
        CHECK(call1.isConstructCall == true);
        CHECK(call1.argumentCount == 0);

        ts_tree_delete(tree);
    }

    TEST_CASE("CallSiteFacts - Enclosing class, mixin, and const method context")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Cls");
        const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("Mix");
        const std::string constMethod = angel_lsp::test::GenerateRandomSymbolName("constMethod");
        const std::string mutMethod = angel_lsp::test::GenerateRandomSymbolName("mutMethod");
        const std::string calleeName = angel_lsp::test::GenerateRandomSymbolName("callee");

        const std::string code = "class " + clsName + "\n{\n" + "    void " + constMethod + "() const\n    {\n" +
                                 "        " + calleeName + "();\n" + "    }\n" + "}\n" + "mixin class " + mixinName +
                                 "\n{\n" + "    void " + mutMethod + "()\n    {\n" + "        " + calleeName + "();\n" +
                                 "    }\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        REQUIRE(facts.callSites.size() == 2);

        // Call inside class const method
        const auto& call0 = facts.callSites[0];
        CHECK(call0.insideClass == true);
        CHECK(call0.insideMixin == false);
        CHECK(facts.TextOf(call0.enclosingClassNameBytes) == clsName);
        CHECK(facts.TextOf(call0.enclosingFunctionNameBytes) == constMethod);
        CHECK(call0.isEnclosingMethodConst == true);

        // Call inside mixin class non-const method
        const auto& call1 = facts.callSites[1];
        CHECK(call1.insideClass == true);
        CHECK(call1.insideMixin == true);
        CHECK(facts.TextOf(call1.enclosingClassNameBytes) == mixinName);
        CHECK(facts.TextOf(call1.enclosingFunctionNameBytes) == mutMethod);
        CHECK(call1.isEnclosingMethodConst == false);

        ts_tree_delete(tree);
    }

    TEST_CASE("CallSiteFacts - Variable direct initialization")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Point");
        const std::string varName = angel_lsp::test::GenerateRandomSymbolName("p");

        const std::string code =
            "void " + fnName + "()\n{\n" + "    " + typeName + " " + varName + "(10, 20);\n" + "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        REQUIRE(facts.directInits.size() == 1);
        const auto& init = facts.directInits[0];
        CHECK(facts.TextOf(init.declaredTypeBytes) == typeName);
        CHECK(init.argumentCount == 2);
        CHECK(facts.FindDirectInit(init.bytes) == &init);

        const auto args = facts.ArgumentsOf(init);
        REQUIRE(args.size() == 2);
        CHECK(facts.TextOf(args[0].bytes) == "10");
        CHECK(facts.TextOf(args[1].bytes) == "20");

        ts_tree_delete(tree);
    }

    TEST_CASE("CallSiteFacts - Direct initializers keep each declarator identifier span")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Point");
        const std::string firstName = angel_lsp::test::GenerateRandomSymbolName("first");
        const std::string secondName = angel_lsp::test::GenerateRandomSymbolName("second");
        const std::string code =
            "void " + fnName + "()\n{\n    " + typeName + "\n    " + firstName + "(1), " + secondName + "(2);\n}\n";

        const auto tree = parser.ParseTree(code);
        REQUIRE(tree != nullptr);
        const angel_lsp::analysis::NodeIndex index(ts_tree_root_node(tree.get()),
                                                   angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        REQUIRE(facts.directInits.size() == 2);
        const auto& first = facts.directInits[0];
        const auto& second = facts.directInits[1];
        CHECK(first.nameBytes.startByte == code.find(firstName));
        CHECK(second.nameBytes.startByte == code.find(secondName));
        CHECK(facts.TextOf(first.nameBytes) == firstName);
        CHECK(facts.TextOf(second.nameBytes) == secondName);
        const auto firstEnd = 4 + static_cast<uint32_t>(firstName.size());
        const auto secondStart = firstEnd + 5; // Skip "(1), ".
        const auto secondEnd = secondStart + static_cast<uint32_t>(secondName.size());
        CHECK(first.nameRange == (angel_lsp::analysis::SourceRange{3, 4, 3, firstEnd}));
        CHECK(second.nameRange == (angel_lsp::analysis::SourceRange{3, secondStart, 3, secondEnd}));
    }

    TEST_CASE("CallSiteFacts - Compact RAM layout, empty-call reserve, and pool slicing")
    {
        CHECK(sizeof(angel_lsp::analysis::CallArgumentTernaryFact) <= 16);
        CHECK(sizeof(angel_lsp::analysis::CallLambdaParamFact) <= 12);
        CHECK(sizeof(angel_lsp::analysis::CallArgumentFact) <= 48);
        CHECK(sizeof(angel_lsp::analysis::CallSiteFact) <= 108);
        CHECK(sizeof(angel_lsp::analysis::DirectInitSiteFact) <= 80);

        angel_lsp::parser::AngelScriptParser parser;
        std::string noCallsCode;
        for (int i = 0; i < 8; ++i)
        {
            noCallsCode += "namespace " + angel_lsp::test::GenerateRandomSymbolName("ns") + " {}\n";
        }
        {
            TSTree* tree = parser.Parse(noCallsCode);
            REQUIRE(tree != nullptr);
            TSNode root = ts_tree_root_node(tree);
            angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = noCallsCode});
            const auto emptyFacts = angel_lsp::analysis::BuildCallSiteFacts(index, noCallsCode);
            CHECK(emptyFacts.callSites.empty());
            CHECK(emptyFacts.directInits.empty());
            CHECK(emptyFacts.arguments.empty());
            CHECK(emptyFacts.ternaryBranches.empty());
            CHECK(emptyFacts.lambdaParams.empty());
            CHECK(emptyFacts.thisMemberNames.empty());
            ts_tree_delete(tree);
        }

        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string calleeA = angel_lsp::test::GenerateRandomSymbolName("callA");
        const std::string calleeB = angel_lsp::test::GenerateRandomSymbolName("callB");
        const std::string argVar = angel_lsp::test::GenerateRandomSymbolName("argV");
        const std::string code = "void " + fnName + "()\n{\n    " + calleeA + "((" + argVar + "));\n    " + calleeB +
                                 "(this, function(const int& in x, y) {});\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree);
        angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        REQUIRE(facts.callSites.size() == 2);
        const auto args0 = facts.ArgumentsOf(facts.callSites[0]);
        REQUIRE(args0.size() == 1);
        CHECK(args0[0].kind == angel_lsp::analysis::CallArgumentKind::Identifier);
        CHECK(facts.TextOf(args0[0].operandBytes) == argVar);
        CHECK(facts.TernaryOf(args0[0]).consequenceBytes.Empty());
        CHECK(facts.LambdaParamsOf(args0[0]).empty());
        CHECK(facts.ThisMemberName(args0[0]).empty());

        const auto args1 = facts.ArgumentsOf(facts.callSites[1]);
        REQUIRE(args1.size() == 2);
        CHECK(args1[0].isThis == true);
        CHECK(args1[1].kind == angel_lsp::analysis::CallArgumentKind::Lambda);
        const auto lparams = facts.LambdaParamsOf(args1[1]);
        REQUIRE(lparams.size() == 2);
        CHECK(lparams[0].hasWrittenType == true);
        CHECK(lparams[0].isConst == true);
        CHECK(lparams[0].isReference == true);
        CHECK(lparams[0].modifier == angel_lsp::analysis::ParameterModifier::In);
        CHECK(lparams[1].hasWrittenType == false);

        ts_tree_delete(tree);
    }
    TEST_CASE("CallSiteFacts - A computed callee is Indirect and keeps its whole expression span")
    {
        angel_lsp::parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string table = angel_lsp::test::GenerateRandomSymbolName("callbacks");
        const std::string maker = angel_lsp::test::GenerateRandomSymbolName("make");
        const std::string code = "void " + fnName + "()\n{\n    " + table + "[0](this);\n    " + maker + "()(1);\n}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        angel_lsp::analysis::NodeIndex index(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});
        const auto facts = angel_lsp::analysis::BuildCallSiteFacts(index, code);

        const auto indexed = std::find_if(facts.callSites.begin(), facts.callSites.end(), [&](const auto& site)
                                          { return facts.TextOf(site.calleeBytes) == table + "[0]"; });
        REQUIRE(indexed != facts.callSites.end());
        CHECK(indexed->calleeKind == angel_lsp::analysis::CalleeKind::Indirect);
        CHECK(indexed->isMemberCall == false);
        CHECK(indexed->argumentCount == 1);

        const auto chained = std::find_if(facts.callSites.begin(), facts.callSites.end(), [&](const auto& site)
                                          { return facts.TextOf(site.calleeBytes) == maker + "()"; });
        REQUIRE(chained != facts.callSites.end());
        CHECK(chained->calleeKind == angel_lsp::analysis::CalleeKind::Indirect);

        ts_tree_delete(tree);
    }
}
