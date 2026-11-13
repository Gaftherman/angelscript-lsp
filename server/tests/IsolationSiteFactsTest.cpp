#include "analysis/index/IsolationSiteFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("IsolationSiteFacts - Shared modifier eligibility and invalid entities")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("SCls");
    const std::string ifaceName = angel_lsp::test::GenerateRandomSymbolName("SIFace");
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("SFn");
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("sVar");

    const std::string code = "shared class " + clsName + " {}\n" + "shared interface " + ifaceName + " {}\n" +
                             "shared void " + fnName + "() {}\n" + "shared int " + varName + ";\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildIsolationSiteFacts(index, code);

    REQUIRE(facts.sharedModifiers.size() == 4);

    // 1. Shared class: valid
    const auto& modCls = facts.sharedModifiers[0];
    CHECK(modCls.isValid == true);
    CHECK(facts.FindSharedModifier(modCls.bytes) == &modCls);

    // 2. Shared interface: valid
    const auto& modIface = facts.sharedModifiers[1];
    CHECK(modIface.isValid == true);
    CHECK(facts.FindSharedModifier(modIface.bytes) == &modIface);

    // 3. Shared function: valid
    const auto& modFn = facts.sharedModifiers[2];
    CHECK(modFn.isValid == true);
    CHECK(facts.FindSharedModifier(modFn.bytes) == &modFn);

    // 4. Shared global variable: invalid
    const auto& modVar = facts.sharedModifiers[3];
    CHECK(modVar.isValid == false);
    CHECK(facts.FindSharedModifier(modVar.bytes) == &modVar);
}

TEST_CASE("IsolationSiteFacts - Class and function shared contexts and member slices")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string sharedCls = angel_lsp::test::GenerateRandomSymbolName("SharedClass");
    const std::string sharedMethA = angel_lsp::test::GenerateRandomSymbolName("methA");
    const std::string sharedMethB = angel_lsp::test::GenerateRandomSymbolName("methB");
    const std::string normalCls = angel_lsp::test::GenerateRandomSymbolName("NormalClass");
    const std::string memberShared = angel_lsp::test::GenerateRandomSymbolName("sharedMethod");
    const std::string memberNormal = angel_lsp::test::GenerateRandomSymbolName("normalMethod");
    const std::string freeShared = angel_lsp::test::GenerateRandomSymbolName("freeShared");
    const std::string freeNormal = angel_lsp::test::GenerateRandomSymbolName("freeNormal");

    const std::string code = "shared class " + sharedCls + " {\n" + "    void " + sharedMethA + "() {}\n" +
                             "    void " + sharedMethB + "() {}\n" + "    int fieldVal;\n" + "}\n" + "class " +
                             normalCls + " {\n" + "    shared void " + memberShared + "() {}\n" + "    void " +
                             memberNormal + "() {}\n" + "}\n" + "shared void " + freeShared + "() {}\n" + "void " +
                             freeNormal + "() {}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildIsolationSiteFacts(index, code);

    REQUIRE(facts.classes.size() == 2);

    // Shared class inspection
    const auto& cls0 = facts.classes[0];
    CHECK(cls0.name == sharedCls);
    CHECK(cls0.isShared);
    CHECK(cls0.functionCount == 2);
    CHECK(cls0.typeCount >= 1);
    CHECK(facts.FindClass(cls0.bytes) == &cls0);

    // Normal class inspection
    const auto& cls1 = facts.classes[1];
    CHECK(cls1.name == normalCls);
    CHECK_FALSE(cls1.isShared);
    CHECK(cls1.functionCount == 2);
    CHECK(facts.FindClass(cls1.bytes) == &cls1);

    REQUIRE(facts.functions.size() == 6);

    // Functions in shared class inherit shared isolation
    const auto& fn0 = facts.functions[cls0.functionBegin];
    CHECK(fn0.name == sharedMethA);
    CHECK(fn0.className == sharedCls);
    CHECK(fn0.isClassShared == true);
    CHECK(fn0.inSharedContext == true);
    CHECK(facts.FindFunction(fn0.bytes) == &fn0);

    const auto& fn1 = facts.functions[cls0.functionBegin + 1];
    CHECK(fn1.name == sharedMethB);
    CHECK(fn1.isClassShared == true);
    CHECK(fn1.inSharedContext == true);

    // Shared member method in normal class
    const auto& fn2 = facts.functions[cls1.functionBegin];
    CHECK(fn2.name == memberShared);
    CHECK(fn2.className == normalCls);
    CHECK(fn2.isShared);
    CHECK(fn2.isClassShared == false);
    CHECK(fn2.inSharedContext == true);

    // Normal member method in normal class
    const auto& fn3 = facts.functions[cls1.functionBegin + 1];
    CHECK(fn3.name == memberNormal);
    CHECK_FALSE(fn3.isShared);
    CHECK(fn3.isClassShared == false);
    CHECK(fn3.inSharedContext == false);

    // Free functions
    const auto* freeSharedFact = facts.FindFunction(facts.functions[4].bytes);
    REQUIRE(freeSharedFact != nullptr);
    CHECK(freeSharedFact->name == freeShared);
    CHECK(freeSharedFact->isShared == true);
    CHECK(freeSharedFact->inSharedContext == true);
    CHECK(freeSharedFact->className.empty());

    const auto* freeNormalFact = facts.FindFunction(facts.functions[5].bytes);
    REQUIRE(freeNormalFact != nullptr);
    CHECK(freeNormalFact->name == freeNormal);
    CHECK(freeNormalFact->isShared == false);
    CHECK(freeNormalFact->inSharedContext == false);
}

TEST_CASE("IsolationSiteFacts - Datatypes, calls, and identifiers classification")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string sFnName = angel_lsp::test::GenerateRandomSymbolName("sFunc");
    const std::string nFnName = angel_lsp::test::GenerateRandomSymbolName("nFunc");
    const std::string customType = angel_lsp::test::GenerateRandomSymbolName("CustomType");
    const std::string calleeDirect = angel_lsp::test::GenerateRandomSymbolName("DirectCall");
    const std::string calleeScoped = angel_lsp::test::GenerateRandomSymbolName("ScopedCall");
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("MyNS");
    const std::string varLocal = angel_lsp::test::GenerateRandomSymbolName("localVar");
    const std::string varGlobal = angel_lsp::test::GenerateRandomSymbolName("globalVar");

    const std::string code = "shared void " + sFnName + "()\n{\n" + "    " + customType + " " + varLocal + ";\n" +
                             "    " + calleeDirect + "();\n" + "    " + nsName + "::" + calleeScoped + "();\n" +
                             "    " + varLocal + " = " + varGlobal + ";\n" + "}\n" + "void " + nFnName + "()\n{\n" +
                             "    " + customType + " obj;\n" + "    " + calleeDirect + "();\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildIsolationSiteFacts(index, code);

    // 1. Verify datatypes
    bool foundSharedType = false;
    bool foundNormalType = false;
    for (const auto& ty : facts.types)
    {
        if (ty.cleanName == customType)
        {
            if (ty.inSharedContext)
            {
                foundSharedType = true;
                CHECK(ty.rawText == customType);
                CHECK(facts.FindType(ty.bytes) == &ty);
            }
            else
            {
                foundNormalType = true;
            }
        }
    }
    CHECK(foundSharedType == true);
    CHECK(foundNormalType == true);

    // 2. Verify calls
    bool foundSharedDirect = false;
    bool foundSharedScoped = false;
    bool foundNormalCall = false;
    for (const auto& call : facts.calls)
    {
        if (call.calleeText == calleeDirect)
        {
            if (call.inSharedContext)
            {
                foundSharedDirect = true;
                CHECK(call.isDirectOrScoped == true);
                CHECK(facts.FindCall(call.bytes) == &call);
            }
            else
            {
                foundNormalCall = true;
            }
        }
        else if (call.calleeText == (nsName + "::" + calleeScoped))
        {
            if (call.inSharedContext)
            {
                foundSharedScoped = true;
                CHECK(call.isDirectOrScoped == true);
            }
        }
    }
    CHECK(foundSharedDirect == true);
    CHECK(foundSharedScoped == true);
    CHECK(foundNormalCall == true);

    // 3. Verify identifiers
    bool foundGlobalRead = false;
    bool foundDeclarator = false;
    for (const auto& id : facts.identifiers)
    {
        if (id.name == varGlobal && id.inSharedContext)
        {
            foundGlobalRead = true;
            CHECK(id.isSpecialContext == false);
            CHECK(id.isCallee == false);
            CHECK(id.isMember == false);
            CHECK(id.isDeclarationName == false);
            CHECK(facts.FindIdentifier(id.bytes) == &id);
        }
        if (id.name == varLocal && id.isDeclarationName)
        {
            foundDeclarator = true;
            CHECK(id.isSpecialContext == true);
        }
    }
    CHECK(foundGlobalRead == true);
    CHECK(foundDeclarator == true);
}
