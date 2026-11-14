#include "analysis/index/CallSiteFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

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
        CHECK(call0.calleeName == funcA);
        CHECK(call0.calleeText == funcA);
        CHECK(call0.isMemberCall == false);
        CHECK(call0.isThisCall == false);
        CHECK(call0.argumentCount == 0);
        CHECK(facts.FindCallSite(call0.bytes) == &call0);

        // 2. Scoped identifier call
        const auto& call1 = facts.callSites[1];
        CHECK(call1.calleeKind == angel_lsp::analysis::CalleeKind::Scoped);
        CHECK(call1.calleeName == funcB);
        CHECK(call1.scopeQualifier == nsName);
        CHECK(call1.calleeText == (nsName + "::" + funcB));
        CHECK(call1.isMemberCall == false);

        // 3. Object member call
        const auto& call2 = facts.callSites[2];
        CHECK(call2.calleeKind == angel_lsp::analysis::CalleeKind::Member);
        CHECK(call2.calleeName == methodName);
        CHECK(call2.isMemberCall == true);
        CHECK(call2.isThisCall == false);
        CHECK(call2.objectOperand.text == objName);

        // 4. this member call
        const auto& call3 = facts.callSites[3];
        CHECK(call3.calleeKind == angel_lsp::analysis::CalleeKind::Member);
        CHECK(call3.calleeName == methodName);
        CHECK(call3.isMemberCall == true);
        CHECK(call3.isThisCall == true);
        CHECK(call3.objectOperand.text == "this");

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
        REQUIRE(facts.arguments.size() >= 7);

        // Arg 0: Number literal
        const auto& arg0 = facts.arguments[call.argumentBegin];
        CHECK(arg0.kind == angel_lsp::analysis::CallArgumentKind::Literal);
        CHECK(arg0.text == "123");
        CHECK(arg0.isNamed == false);
        CHECK(arg0.operand.isNumericLiteral == true);

        // Arg 1: String literal
        const auto& arg1 = facts.arguments[call.argumentBegin + 1];
        CHECK(arg1.kind == angel_lsp::analysis::CallArgumentKind::Literal);
        CHECK(arg1.text == "\"test\"");

        // Arg 2: Null literal
        const auto& arg2 = facts.arguments[call.argumentBegin + 2];
        CHECK(arg2.kind == angel_lsp::analysis::CallArgumentKind::Literal);
        CHECK(arg2.operand.isNullLiteral == true);

        // Arg 3: Named argument
        const auto& arg3 = facts.arguments[call.argumentBegin + 3];
        CHECK(arg3.isNamed == true);
        CHECK(arg3.name == paramName);
        CHECK(arg3.text == varA);

        // Arg 4: Initializer list
        const auto& arg4 = facts.arguments[call.argumentBegin + 4];
        CHECK(arg4.kind == angel_lsp::analysis::CallArgumentKind::InitializerList);

        // Arg 5: Lambda
        const auto& arg5 = facts.arguments[call.argumentBegin + 5];
        CHECK(arg5.kind == angel_lsp::analysis::CallArgumentKind::Lambda);
        CHECK(arg5.funcdefValue.isLambda == true);

        // Arg 6: Ternary conditional expression
        const auto& arg6 = facts.arguments[call.argumentBegin + 6];
        CHECK(arg6.kind == angel_lsp::analysis::CallArgumentKind::Ternary);
        CHECK(arg6.isTernary == true);
        CHECK(arg6.ternaryConsequence.text == varA);
        CHECK(arg6.ternaryAlternative.text == varC);

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
        CHECK(facts.arguments[c1.argumentBegin].thisMemberName == callDelegate);

        ts_tree_delete(tree);
    }

    TEST_CASE("CallSiteFacts - Construct call expression and template arguments")
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
        CHECK(call0.hasTemplateArguments == true);
        CHECK(call0.templateArgsText == "<int>");
        CHECK(call0.argumentCount == 1);

        const auto& call1 = facts.callSites[1];
        CHECK(call1.isConstructCall == true);
        CHECK(call1.hasTemplateArguments == true);
        CHECK(call1.templateArgsText == "<string>");
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
        CHECK(call0.enclosingClassName == clsName);
        CHECK(call0.enclosingFunctionName == constMethod);
        CHECK(call0.isEnclosingMethodConst == true);

        // Call inside mixin class non-const method
        const auto& call1 = facts.callSites[1];
        CHECK(call1.insideClass == true);
        CHECK(call1.insideMixin == true);
        CHECK(call1.enclosingClassName == mixinName);
        CHECK(call1.enclosingFunctionName == mutMethod);
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
        CHECK(init.declaredType == typeName);
        CHECK(init.baseTypeName == typeName);
        CHECK(init.declaratorName == varName);
        CHECK(init.argumentCount == 2);
        CHECK(facts.FindDirectInit(init.bytes) == &init);

        REQUIRE(facts.arguments.size() >= 2);
        CHECK(facts.arguments[init.argumentBegin].text == "10");
        CHECK(facts.arguments[init.argumentBegin + 1].text == "20");

        ts_tree_delete(tree);
    }
}
