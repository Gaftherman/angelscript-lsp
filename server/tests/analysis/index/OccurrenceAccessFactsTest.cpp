#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/OccurrenceAccess.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{

struct OccurrenceTestContext
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;
    std::string uri = "file:///test_occurrence.as";
    std::string source;
    TSTree* tree = nullptr;
    std::shared_ptr<const Scope> rootScope;

    explicit OccurrenceTestContext(std::string code) : source(std::move(code))
    {
        tree = parser.Parse(source);
        symbolCollector.CollectSymbols(uri, source, parser, symbolTable);
        auto root = scopeCollector.CollectScopes(source, parser);
        if (root)
        {
            scopeIndex.SetScopeTree(uri, std::move(root));
            rootScope = scopeIndex.GetRoot(uri);
        }
    }

    ~OccurrenceTestContext()
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }

    [[nodiscard]] const LocalReference* FindReference(std::string_view name) const
    {
        if (!rootScope)
        {
            return nullptr;
        }
        std::vector<const Scope*> queue = {rootScope.get()};
        while (!queue.empty())
        {
            const Scope* cur = queue.back();
            queue.pop_back();
            for (const auto& ref : cur->references)
            {
                if (ref.name == name)
                {
                    return &ref;
                }
            }
            for (const auto& child : cur->children)
            {
                queue.push_back(child.get());
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<const LocalReference*> FindAllReferences(std::string_view name) const
    {
        std::vector<const LocalReference*> results;
        if (!rootScope)
        {
            return results;
        }
        std::vector<const Scope*> queue = {rootScope.get()};
        while (!queue.empty())
        {
            const Scope* cur = queue.back();
            queue.pop_back();
            for (const auto& ref : cur->references)
            {
                if (ref.name == name)
                {
                    results.push_back(&ref);
                }
            }
            for (const auto& child : cur->children)
            {
                queue.push_back(child.get());
            }
        }
        return results;
    }
};

} // namespace

TEST_SUITE("analysis/index/OccurrenceAccessFacts")
{
    TEST_CASE("OccurrenceAccess - LocalReference isWriteTarget for mutations and assignments")
    {
        const std::string varAssign = angel_lsp::test::GenerateRandomSymbolName("varAssign");
        const std::string varPlusEq = angel_lsp::test::GenerateRandomSymbolName("varPlusEq");
        const std::string varPreInc = angel_lsp::test::GenerateRandomSymbolName("varPreInc");
        const std::string varPostInc = angel_lsp::test::GenerateRandomSymbolName("varPostInc");
        const std::string objVar = angel_lsp::test::GenerateRandomSymbolName("objVar");
        const std::string fieldVar = angel_lsp::test::GenerateRandomSymbolName("fieldVar");
        const std::string varNeg = angel_lsp::test::GenerateRandomSymbolName("varNeg");
        const std::string varNot = angel_lsp::test::GenerateRandomSymbolName("varNot");
        const std::string varEqA = angel_lsp::test::GenerateRandomSymbolName("varEqA");
        const std::string varEqB = angel_lsp::test::GenerateRandomSymbolName("varEqB");

        std::string code =
            "class ObjType { int " + fieldVar + "; }\n" + "void TestFunc() {\n" + "    int " + varAssign + " = 0;\n" +
            "    " + varAssign + " = 42;\n" + "    int " + varPlusEq + " = 10;\n" + "    " + varPlusEq + " += 5;\n" +
            "    int " + varPreInc + " = 1;\n" + "    ++" + varPreInc + ";\n" + "    int " + varPostInc + " = 2;\n" +
            "    " + varPostInc + "++;\n" + "    ObjType " + objVar + ";\n" + "    " + objVar + "." + fieldVar +
            " = 99;\n" + "    int " + varNeg + " = 5;\n" + "    int negResult = -" + varNeg + ";\n" + "    bool " +
            varNot + " = true;\n" + "    bool notResult = !" + varNot + ";\n" + "    int " + varEqA + " = 1;\n" +
            "    int " + varEqB + " = 2;\n" + "    bool eqResult = (" + varEqA + " == " + varEqB + ");\n" + "}\n";

        OccurrenceTestContext ctx(code);
        REQUIRE(bool(ctx.rootScope));

        // 1. Direct assignment
        const auto* refAssign = ctx.FindReference(varAssign);
        REQUIRE(bool(refAssign));
        CHECK(refAssign->isWriteTarget == 1);

        // 2. Compound assignment
        const auto* refPlusEq = ctx.FindReference(varPlusEq);
        REQUIRE(bool(refPlusEq));
        CHECK(refPlusEq->isWriteTarget == 1);

        // 3. Prefix increment
        const auto* refPreInc = ctx.FindReference(varPreInc);
        REQUIRE(bool(refPreInc));
        CHECK(refPreInc->isWriteTarget == 1);

        // 4. Postfix increment
        const auto* refPostInc = ctx.FindReference(varPostInc);
        REQUIRE(bool(refPostInc));
        CHECK(refPostInc->isWriteTarget == 1);

        // 5. Member assignment: obj.field = 99
        // fieldVar must be marked as write target, but objVar must NOT be marked
        const auto* refField = ctx.FindReference(fieldVar);
        REQUIRE(bool(refField));
        CHECK(refField->isWriteTarget == 1);

        const auto* refObj = ctx.FindReference(objVar);
        REQUIRE(bool(refObj));
        CHECK(refObj->isWriteTarget == 0);

        // 6. Non-writes: -var, !var, a == b
        const auto* refNeg = ctx.FindReference(varNeg);
        REQUIRE(bool(refNeg));
        CHECK(refNeg->isWriteTarget == 0);

        const auto* refNot = ctx.FindReference(varNot);
        REQUIRE(bool(refNot));
        CHECK(refNot->isWriteTarget == 0);

        const auto* refEqA = ctx.FindReference(varEqA);
        REQUIRE(bool(refEqA));
        CHECK(refEqA->isWriteTarget == 0);

        const auto* refEqB = ctx.FindReference(varEqB);
        REQUIRE(bool(refEqB));
        CHECK(refEqB->isWriteTarget == 0);
    }

    TEST_CASE("OccurrenceAccess - IndexedCallArgumentRef facts indexing and span matching")
    {
        const std::string funcName = angel_lsp::test::GenerateRandomSymbolName("fn");
        const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("method");
        const std::string objName = angel_lsp::test::GenerateRandomSymbolName("obj");
        const std::string argOne = angel_lsp::test::GenerateRandomSymbolName("argOne");
        const std::string argTwo = angel_lsp::test::GenerateRandomSymbolName("argTwo");
        const std::string fieldName = angel_lsp::test::GenerateRandomSymbolName("memberField");

        std::string code = "class Container { void " + methodName + "(int a, int b) {} }\n" + "void " + funcName +
                           "(int p) {}\n" + "class Wrapper { int " + fieldName + "; }\n" + "void Caller() {\n" +
                           "    Container " + objName + ";\n" + "    int " + argOne + " = 1;\n" + "    int " + argTwo +
                           " = 2;\n" + "    " + objName + "." + methodName + "(" + argOne + ", " + argTwo + ");\n" +
                           "    Wrapper wrap;\n" + "    " + funcName + "(wrap." + fieldName + ");\n" + "}\n";

        OccurrenceTestContext ctx(code);
        REQUIRE(bool(ctx.rootScope));
        REQUIRE(bool(ctx.rootScope->enclosingContext));
        REQUIRE(bool(ctx.rootScope->enclosingContext->receivers));

        const std::string_view srcView(ctx.source);
        const auto& callArgs = ctx.rootScope->enclosingContext->receivers->callArguments;
        REQUIRE(callArgs.size() >= 3);

        // Verify obj.method(argOne, argTwo) arguments
        bool foundArg0 = false;
        bool foundArg1 = false;
        for (const auto& arg : callArgs)
        {
            if (arg.calleeStartByte < arg.calleeEndByte)
            {
                std::string_view callee = srcView.substr(arg.calleeStartByte, arg.calleeEndByte - arg.calleeStartByte);
                if (callee == methodName)
                {
                    std::string_view receiver =
                        srcView.substr(arg.receiverStartByte, arg.receiverEndByte - arg.receiverStartByte);
                    CHECK(receiver == objName);
                    if (arg.argumentIndex == 0)
                    {
                        foundArg0 = true;
                        std::string_view argText = srcView.substr(arg.argStartByte, arg.argEndByte - arg.argStartByte);
                        CHECK(argText == argOne);
                    }
                    else if (arg.argumentIndex == 1)
                    {
                        foundArg1 = true;
                        std::string_view argText = srcView.substr(arg.argStartByte, arg.argEndByte - arg.argStartByte);
                        CHECK(argText == argTwo);
                    }
                }
            }
        }
        CHECK(foundArg0);
        CHECK(foundArg1);

        // Verify f(wrap.fieldName) argument: argument span covers wrap.fieldName
        bool foundMemberArg = false;
        for (const auto& arg : callArgs)
        {
            if (arg.calleeStartByte < arg.calleeEndByte)
            {
                std::string_view callee = srcView.substr(arg.calleeStartByte, arg.calleeEndByte - arg.calleeStartByte);
                if (callee == funcName)
                {
                    foundMemberArg = true;
                    CHECK(arg.argumentIndex == 0);
                    std::string_view argText = srcView.substr(arg.argStartByte, arg.argEndByte - arg.argStartByte);
                    CHECK(argText == "wrap." + fieldName);

                    // Check that fieldName's expression span matches arg span
                    const auto* refMember = ctx.FindReference(fieldName);
                    REQUIRE(bool(refMember));
                    CHECK(refMember->qualifierStartByte == arg.argStartByte);
                    CHECK(refMember->qualifierEndByte == arg.argEndByte);

                    // Check that wrap's expression span does NOT match arg span
                    const auto* refWrap = ctx.FindReference("wrap");
                    REQUIRE(bool(refWrap));
                    CHECK_FALSE(static_cast<bool>(refWrap->isMemberAccess));
                    const bool wrapSpansArgument =
                        refWrap->startByte == arg.argStartByte && refWrap->endByte == arg.argEndByte;
                    CHECK_FALSE(wrapSpansArgument);
                }
            }
        }
        CHECK(foundMemberArg);
    }

    TEST_CASE("OccurrenceAccess - Mutable out/inout reference parameter write classification")
    {
        const std::string outFunc = angel_lsp::test::GenerateRandomSymbolName("MutateOut");
        const std::string constFunc = angel_lsp::test::GenerateRandomSymbolName("ReadOnlyIn");
        const std::string localVar = angel_lsp::test::GenerateRandomSymbolName("targetVar");

        std::string code = "void " + outFunc + "(int &out value) {}\n" + "void " + constFunc +
                           "(const int &in value) {}\n" + "void Runner() {\n" + "    int " + localVar + " = 100;\n" +
                           "    " + outFunc + "(" + localVar + ");\n" + "    " + constFunc + "(" + localVar + ");\n" +
                           "}\n";

        OccurrenceTestContext ctx(code);
        REQUIRE(bool(ctx.rootScope));

        OccurrenceAccessClassifier classifier(ctx.uri, ctx.source, ctx.symbolTable, ctx.scopeIndex);

        // Find occurrences of localVar
        const auto refs = ctx.FindAllReferences(localVar);
        REQUIRE(refs.size() == 2);

        TargetDescriptor target;
        target.name = localVar;
        target.kind = TargetKind::Local;

        // First call: MutateOut(localVar) -> mutable out ref -> Write
        SourceRange rangeOut{refs[0]->startLine, refs[0]->startCharacter, refs[0]->endLine, refs[0]->endCharacter};
        OccurrenceAccess accessOut = classifier.Classify(target, rangeOut);
        CHECK(static_cast<uint8_t>(accessOut) == static_cast<uint8_t>(OccurrenceAccess::Write));

        // Second call: ReadOnlyIn(localVar) -> const in ref -> Read
        SourceRange rangeIn{refs[1]->startLine, refs[1]->startCharacter, refs[1]->endLine, refs[1]->endCharacter};
        OccurrenceAccess accessIn = classifier.Classify(target, rangeIn);
        CHECK(static_cast<uint8_t>(accessIn) == static_cast<uint8_t>(OccurrenceAccess::Read));
    }
}
