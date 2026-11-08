#include "analysis/index/CodeActionFacts.h"

#include "analysis/index/AccessorFacts.h"
#include "analysis/index/ClassMutationFacts.h"
#include "analysis/index/ConstQualifierFacts.h"
#include "analysis/index/ExtractMethodFacts.h"
#include "analysis/index/ExtractVariableFacts.h"
#include "analysis/index/RepeatedConversionFacts.h"
#include "document/Document.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/SyntaxTree.h"
#include "utils/Utils.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <string_view>

TEST_CASE("CodeActionFacts - node span at point")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fn = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string v = angel_lsp::test::GenerateRandomSymbolName("v");
    const std::string code = "void " + fn + "() { int " + v + " = 1; }";
    angel_lsp::parser::TreePtr tree = angel_lsp::parser::MakeTreePtr(parser.Parse(code));
    angel_lsp::test::NodeIndexFixture fx(tree.get());

    const auto vCol = static_cast<uint32_t>(code.find(v));
    const auto vEndCol = static_cast<uint32_t>(vCol + v.size());
    const auto span = angel_lsp::analysis::FindNodeSpanAtPoint(fx.index, 0, vCol);
    REQUIRE(span.has_value());
    const std::string_view spanText = angel_lsp::utils::SliceSource(code, span->bytes.startByte, span->bytes.endByte);
    CHECK(std::string(spanText) == v);
    CHECK(span->range.startLine == 0);
    CHECK(span->range.endLine == 0);
    CHECK(span->range.startCharacter == vCol);
    CHECK(span->range.endCharacter == vEndCol);

    angel_lsp::test::NodeIndexFixture empty(nullptr);
    CHECK_FALSE(angel_lsp::analysis::FindNodeSpanAtPoint(empty.index, 0, vCol).has_value());
}

TEST_CASE("CodeActionFacts - unused declarator removal")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string b = angel_lsp::test::GenerateRandomSymbolName("b");
    const std::string c = angel_lsp::test::GenerateRandomSymbolName("c");

    const std::string singleCode = "void f() {\n  int " + a + " = 1;\n}";
    angel_lsp::parser::TreePtr singleTree = angel_lsp::parser::MakeTreePtr(parser.Parse(singleCode));
    angel_lsp::test::NodeIndexFixture singleFx(singleTree.get());

    const auto singleLine1Start = singleCode.find('\n') + 1;
    const auto singleACol = static_cast<uint32_t>(singleCode.find(a) - singleLine1Start);
    const auto singleResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(singleFx.index, 1, singleACol);
    CHECK(singleResult.inDeclaration);
    CHECK(singleResult.declaratorCount == 1);

    const std::string multiCode = "void f() {\n  int " + a + " = 1, " + b + " = 2, " + c + " = 3;\n}";
    angel_lsp::parser::TreePtr multiTree = angel_lsp::parser::MakeTreePtr(parser.Parse(multiCode));
    angel_lsp::test::NodeIndexFixture multiFx(multiTree.get());

    const auto multiLine1Start = multiCode.find('\n') + 1;
    const auto multiACol = static_cast<uint32_t>(multiCode.find(a) - multiLine1Start);
    const auto multiBCol = static_cast<uint32_t>(multiCode.find(b) - multiLine1Start);

    const std::string declA = a + " = 1";
    const std::string declB = b + " = 2";
    const auto declAStartCol = static_cast<uint32_t>(multiCode.find(declA) - multiLine1Start);
    const auto declBStartCol = static_cast<uint32_t>(multiCode.find(declB) - multiLine1Start);
    const auto declAEndCol = static_cast<uint32_t>(multiCode.find(declA) + declA.size() - multiLine1Start);
    const auto declBEndCol = static_cast<uint32_t>(multiCode.find(declB) + declB.size() - multiLine1Start);

    const auto aResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(multiFx.index, 1, multiACol);
    CHECK(aResult.inDeclaration);
    CHECK(aResult.declaratorCount == 3);
    CHECK(aResult.multiRemovalRange.startLine == 1);
    CHECK(aResult.multiRemovalRange.startCharacter == declAStartCol);
    CHECK(aResult.multiRemovalRange.endLine == 1);
    CHECK(aResult.multiRemovalRange.endCharacter == declBStartCol);

    const auto bResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(multiFx.index, 1, multiBCol);
    CHECK(bResult.inDeclaration);
    CHECK(bResult.declaratorCount == 3);
    CHECK(bResult.multiRemovalRange.startLine == 1);
    CHECK(bResult.multiRemovalRange.startCharacter == declAEndCol);
    CHECK(bResult.multiRemovalRange.endLine == 1);
    CHECK(bResult.multiRemovalRange.endCharacter == declBEndCol);

    const auto fnCol = static_cast<uint32_t>(multiCode.find("f"));
    const auto fnResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(multiFx.index, 0, fnCol);
    CHECK_FALSE(fnResult.inDeclaration);
    CHECK(fnResult.declaratorCount == 0);

    angel_lsp::test::NodeIndexFixture empty(nullptr);
    const auto emptyResult = angel_lsp::analysis::FindUnusedDeclaratorRemoval(empty.index, 0, 0);
    CHECK_FALSE(emptyResult.inDeclaration);
}

TEST_CASE("CodeActionFacts - extract variable site")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fn = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string v = angel_lsp::test::GenerateRandomSymbolName("v");
    const std::string callee = angel_lsp::test::GenerateRandomSymbolName("callee");
    const std::string code1 = "void " + fn + "() {\n  int " + v + " = " + callee + "(1);\n}";
    angel_lsp::parser::TreePtr tree1 = angel_lsp::parser::MakeTreePtr(parser.Parse(code1));
    angel_lsp::test::NodeIndexFixture fx1(tree1.get());

    const std::string callExpr = callee + "(1)";
    const auto line1Start1 = code1.find('\n') + 1;
    const auto callCol = static_cast<uint32_t>(code1.find(callExpr) - line1Start1);
    const auto callEndCol = static_cast<uint32_t>(code1.find(callExpr) + callExpr.size() - line1Start1);
    const angel_lsp::analysis::SourceRange selection1{1, callCol, 1, callEndCol};

    const auto site1 = angel_lsp::analysis::FindExtractVariableSite(fx1.index, selection1);
    REQUIRE(site1.has_value());
    const std::string_view exprSlice1 =
        angel_lsp::utils::SliceSource(code1, site1->expression.startByte, site1->expression.endByte);
    CHECK(std::string(exprSlice1) == callExpr);
    CHECK(site1->statementLine == 1);
    CHECK(site1->hintKind == angel_lsp::analysis::ExtractNameHintKind::CallCallee);
    const std::string_view hintSlice1 =
        angel_lsp::utils::SliceSource(code1, site1->hintName.startByte, site1->hintName.endByte);
    CHECK(std::string(hintSlice1) == callee);

    const std::string fn2 = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string v2 = angel_lsp::test::GenerateRandomSymbolName("v");
    const std::string obj = angel_lsp::test::GenerateRandomSymbolName("obj");
    const std::string mem = angel_lsp::test::GenerateRandomSymbolName("mem");
    const std::string memberExpr = obj + "." + mem;
    const std::string code2 = "void " + fn2 + "() {\n  int " + v2 + " = " + memberExpr + ";\n}";
    angel_lsp::parser::TreePtr tree2 = angel_lsp::parser::MakeTreePtr(parser.Parse(code2));
    angel_lsp::test::NodeIndexFixture fx2(tree2.get());

    const auto line1Start2 = code2.find('\n') + 1;
    const auto memberCol = static_cast<uint32_t>(code2.find(memberExpr) - line1Start2);
    const auto memberEndCol = static_cast<uint32_t>(code2.find(memberExpr) + memberExpr.size() - line1Start2);
    const angel_lsp::analysis::SourceRange selection2{1, memberCol, 1, memberEndCol};

    const auto site2 = angel_lsp::analysis::FindExtractVariableSite(fx2.index, selection2);
    REQUIRE(site2.has_value());
    CHECK(site2->hintKind == angel_lsp::analysis::ExtractNameHintKind::Member);
    const std::string_view hintSlice2 =
        angel_lsp::utils::SliceSource(code2, site2->hintName.startByte, site2->hintName.endByte);
    CHECK(std::string(hintSlice2) == mem);

    angel_lsp::test::NodeIndexFixture emptyFx(nullptr);
    CHECK_FALSE(angel_lsp::analysis::FindExtractVariableSite(emptyFx.index, selection1).has_value());
}

TEST_CASE("CodeActionFacts - extract method selection")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string cls = angel_lsp::test::GenerateRandomSymbolName("cls");
    const std::string m = angel_lsp::test::GenerateRandomSymbolName("m");
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string code = "class " + cls + " {\n  int " + m + "() {\n    int " + a + " = 1;\n    " + a +
                             " = 2;\n    return " + a + ";\n  }\n}";
    angel_lsp::parser::TreePtr tree = angel_lsp::parser::MakeTreePtr(parser.Parse(code));
    angel_lsp::test::NodeIndexFixture fx(tree.get());

    const std::string retStmt = "    return " + a + ";";
    const auto line4Start = code.find(retStmt);
    const auto line4End = code.find('\n', line4Start);
    const auto line4EndCol = static_cast<uint32_t>(line4End - line4Start);
    const angel_lsp::analysis::SourceRange selection{2, 0, 4, line4EndCol};

    const auto result = angel_lsp::analysis::FindExtractMethodSelection(fx.index, code, selection);
    REQUIRE(result.has_value());
    CHECK(result->inClass);
    CHECK(result->endsWithReturn);
    const std::string_view retTypeSlice =
        angel_lsp::utils::SliceSource(code, result->functionReturnType.startByte, result->functionReturnType.endByte);
    CHECK(std::string(retTypeSlice) == "int");
    CHECK(result->statementsRange.startLine == 2);
    CHECK(result->statementsRange.endLine == 4);

    bool foundMutatedA = false;
    for (const auto& mutatedRange : result->mutatedNames)
    {
        const std::string_view slice =
            angel_lsp::utils::SliceSource(code, mutatedRange.startByte, mutatedRange.endByte);
        if (std::string(slice) == a)
        {
            foundMutatedA = true;
            break;
        }
    }
    CHECK(foundMutatedA);

    const std::string freeFn = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string freeA = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string freeCode = "void " + freeFn + "() {\n  int " + freeA + " = 1;\n  " + freeA + " = 2;\n}";
    angel_lsp::parser::TreePtr freeTree = angel_lsp::parser::MakeTreePtr(parser.Parse(freeCode));
    angel_lsp::test::NodeIndexFixture freeFx(freeTree.get());

    const std::string freeLine2 = "  " + freeA + " = 2;";
    const auto freeLine2Start = freeCode.find(freeLine2);
    const auto freeLine2End = freeCode.find('\n', freeLine2Start);
    const auto freeLine2EndCol = static_cast<uint32_t>(freeLine2End - freeLine2Start);
    const angel_lsp::analysis::SourceRange freeSelection{1, 0, 2, freeLine2EndCol};

    const auto freeResult = angel_lsp::analysis::FindExtractMethodSelection(freeFx.index, freeCode, freeSelection);
    REQUIRE(freeResult.has_value());
    CHECK_FALSE(freeResult->inClass);
    CHECK_FALSE(freeResult->endsWithReturn);

    const angel_lsp::analysis::SourceRange emptySelection{1, 0, 1, 0};
    CHECK_FALSE(angel_lsp::analysis::FindExtractMethodSelection(freeFx.index, freeCode, emptySelection).has_value());
}

TEST_CASE("CodeActionFacts - accessor site")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string cls = angel_lsp::test::GenerateRandomSymbolName("cls");
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string b = angel_lsp::test::GenerateRandomSymbolName("b");
    const std::string source = "class " + cls + " {\n  int " + a + ", " + b + ";\n}";
    angel_lsp::parser::TreePtr tree = angel_lsp::parser::MakeTreePtr(parser.Parse(source));
    angel_lsp::test::NodeIndexFixture fx(tree.get());

    const auto line1Start = source.find('\n') + 1;
    const auto aCol = static_cast<uint32_t>(source.find(a, line1Start) - line1Start);
    const auto site = angel_lsp::analysis::FindAccessorSite(fx.index, source, 1, aCol);
    REQUIRE(site.has_value());

    const std::string_view classSlice =
        angel_lsp::utils::SliceSource(source, site->className.startByte, site->className.endByte);
    CHECK(std::string(classSlice) == cls);

    const std::string_view fieldTypeSlice =
        angel_lsp::utils::SliceSource(source, site->fieldType.startByte, site->fieldType.endByte);
    CHECK(std::string(fieldTypeSlice) == "int");

    REQUIRE(site->fieldNames.size() == 2);
    const std::string_view aSlice =
        angel_lsp::utils::SliceSource(source, site->fieldNames[0].startByte, site->fieldNames[0].endByte);
    CHECK(std::string(aSlice) == a);
    const std::string_view bSlice =
        angel_lsp::utils::SliceSource(source, site->fieldNames[1].startByte, site->fieldNames[1].endByte);
    CHECK(std::string(bSlice) == b);

    const auto line2Start = source.rfind('\n') + 1;
    const auto closeBraceCol = static_cast<uint32_t>(source.find('}', line2Start) - line2Start);
    CHECK(site->insertPoint.line == 2);
    CHECK(site->insertPoint.character == closeBraceCol);
    CHECK(site->insertPoint.character == 0);

    const auto clsCol = static_cast<uint32_t>(source.find(cls));
    CHECK_FALSE(angel_lsp::analysis::FindAccessorSite(fx.index, source, 0, clsCol).has_value());

    angel_lsp::test::NodeIndexFixture emptyFx(nullptr);
    CHECK_FALSE(angel_lsp::analysis::FindAccessorSite(emptyFx.index, source, 1, aCol).has_value());
}

TEST_CASE("CodeActionFacts - property keyword insertion")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string cls = angel_lsp::test::GenerateRandomSymbolName("cls");
    const std::string p = angel_lsp::test::GenerateRandomSymbolName("p");
    const std::string getter = "get_" + p;

    const std::string sourceConst = "class " + cls + " {\n  int " + getter + "() const { return 0; }\n}";
    angel_lsp::parser::TreePtr treeConst = angel_lsp::parser::MakeTreePtr(parser.Parse(sourceConst));
    angel_lsp::test::NodeIndexFixture fxConst(treeConst.get());

    const auto line1StartConst = sourceConst.find('\n') + 1;
    const auto getterColConst = static_cast<uint32_t>(sourceConst.find(getter, line1StartConst) - line1StartConst);
    const auto constPos = sourceConst.find("const", line1StartConst);
    const auto afterConstCol = static_cast<uint32_t>((constPos + std::string("const").size()) - line1StartConst);

    const auto ptConst = angel_lsp::analysis::FindPropertyKeywordInsertionPoint(fxConst.index, 1, getterColConst);
    REQUIRE(ptConst.has_value());
    CHECK(ptConst->line == 1);
    CHECK(ptConst->character == afterConstCol);

    const std::string clsNoConst = angel_lsp::test::GenerateRandomSymbolName("cls");
    const std::string pNoConst = angel_lsp::test::GenerateRandomSymbolName("p");
    const std::string getterNoConst = "get_" + pNoConst;

    const std::string sourceNoConst = "class " + clsNoConst + " {\n  int " + getterNoConst + "() { return 0; }\n}";
    angel_lsp::parser::TreePtr treeNoConst = angel_lsp::parser::MakeTreePtr(parser.Parse(sourceNoConst));
    angel_lsp::test::NodeIndexFixture fxNoConst(treeNoConst.get());

    const auto line1StartNoConst = sourceNoConst.find('\n') + 1;
    const auto getterColNoConst =
        static_cast<uint32_t>(sourceNoConst.find(getterNoConst, line1StartNoConst) - line1StartNoConst);
    const auto closeParenPos = sourceNoConst.find(')', line1StartNoConst);
    const auto afterParenCol = static_cast<uint32_t>((closeParenPos + 1) - line1StartNoConst);

    const auto ptNoConst = angel_lsp::analysis::FindPropertyKeywordInsertionPoint(fxNoConst.index, 1, getterColNoConst);
    REQUIRE(ptNoConst.has_value());
    CHECK(ptNoConst->line == 1);
    CHECK(ptNoConst->character == afterParenCol);
}

TEST_CASE("CodeActionFacts - class open brace point")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string cls = angel_lsp::test::GenerateRandomSymbolName("cls");
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string source = "class " + cls + "\n{\n  int " + a + ";\n}";
    angel_lsp::parser::TreePtr tree = angel_lsp::parser::MakeTreePtr(parser.Parse(source));
    angel_lsp::test::NodeIndexFixture fx(tree.get());

    const auto line1Start = source.find('\n') + 1;
    const auto braceCol = static_cast<uint32_t>((source.find('{', line1Start) - line1Start) + 1);

    const auto pt = angel_lsp::analysis::FindClassOpenBracePoint(fx.index, 0, 0);
    REQUIRE(pt.has_value());
    CHECK(pt->line == 1);
    CHECK(pt->character == braceCol);
    CHECK(pt->character == 1);
}

TEST_CASE("CodeActionFacts - if ladder conversion sites")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fn = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string x = angel_lsp::test::GenerateRandomSymbolName("x");
    const std::string source =
        "void " + fn + "(int " + x + ") {\n  if (" + x + " == 1) {}\n  else if (" + x + " == 2) {}\n}";
    angel_lsp::parser::TreePtr tree = angel_lsp::parser::MakeTreePtr(parser.Parse(source));
    angel_lsp::test::NodeIndexFixture fx(tree.get());

    const auto line1Start = source.find('\n') + 1;
    const auto xPosLine1 = source.find(x, line1Start);
    const auto xStartCol = static_cast<uint32_t>(xPosLine1 - line1Start);
    const auto xEndCol = static_cast<uint32_t>(xStartCol + x.size());
    const angel_lsp::analysis::SourceRange diagnosticRange{1, xStartCol, 1, xEndCol};

    const auto sites = angel_lsp::analysis::FindIfLadderConversionSites(fx.index, source, diagnosticRange, x);
    REQUIRE(sites.has_value());
    CHECK(sites->rootIfLine == 1);
    REQUIRE(sites->occurrences.size() == 2);
    CHECK(sites->occurrences[0].startLine == 1);
    CHECK(sites->occurrences[1].startLine == 2);

    const auto paramPos = source.find(x);
    const auto paramStartCol = static_cast<uint32_t>(paramPos);
    const auto paramEndCol = static_cast<uint32_t>(paramStartCol + x.size());
    const angel_lsp::analysis::SourceRange paramRange{0, paramStartCol, 0, paramEndCol};
    CHECK_FALSE(angel_lsp::analysis::FindIfLadderConversionSites(fx.index, source, paramRange, x).has_value());
}

TEST_CASE("CodeActionFacts - const qualifier sites")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string cls = angel_lsp::test::GenerateRandomSymbolName("cls");
    const std::string f = angel_lsp::test::GenerateRandomSymbolName("f");
    const std::string get = angel_lsp::test::GenerateRandomSymbolName("get");
    const std::string set = angel_lsp::test::GenerateRandomSymbolName("set");
    const std::string fn = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string h = angel_lsp::test::GenerateRandomSymbolName("h");

    const std::string source = "class " + cls + " {\n  int " + f + ";\n  int " + get + "() { return " + f +
                               "; }\n  void " + set + "(int v) { " + f + " = v; " + f + "++; this." + f + " = v; " +
                               get + "(); }\n}\nvoid " + fn + "(" + cls + "@ " + h + ") { " + h + "." + get + "(); }";
    angel_lsp::parser::TreePtr tree = angel_lsp::parser::MakeTreePtr(parser.Parse(source));
    angel_lsp::test::NodeIndexFixture fx(tree.get());

    const auto line1Start = source.find('\n') + 1;
    const auto line2Start = source.find('\n', line1Start) + 1;
    const auto line3Start = source.find('\n', line2Start) + 1;
    const auto line4Start = source.find('\n', line3Start) + 1;
    const auto line5Start = source.find('\n', line4Start) + 1;

    const auto getColLine2 = static_cast<uint32_t>(source.find(get, line2Start) - line2Start);
    const auto paramListEndCol = static_cast<uint32_t>((source.find("()", line2Start) + 2) - line2Start);
    const auto siteGet = angel_lsp::analysis::FindIntentionalConstSite(fx.index, source, 2, getColLine2);
    REQUIRE(siteGet.has_value());
    const std::string_view classSlice =
        angel_lsp::utils::SliceSource(source, siteGet->className.startByte, siteGet->className.endByte);
    CHECK(std::string(classSlice) == cls);
    CHECK(siteGet->methodStartLine == 2);
    CHECK(siteGet->parameterListEnd.line == 2);
    CHECK(siteGet->parameterListEnd.character == paramListEndCol);
    CHECK(siteGet->mutation.bodyStartLine == 2);

    const auto setColLine3 = static_cast<uint32_t>(source.find(set, line3Start) - line3Start);
    const auto siteSet = angel_lsp::analysis::FindIntentionalConstSite(fx.index, source, 3, setColLine3);
    REQUIRE(siteSet.has_value());
    REQUIRE(siteSet->mutation.sites.size() == 4);
    CHECK(siteSet->mutation.sites[0].kind == angel_lsp::analysis::MutationSiteKind::NamedTarget);
    const std::string_view slice0 = angel_lsp::utils::SliceSource(source, siteSet->mutation.sites[0].name.startByte,
                                                                  siteSet->mutation.sites[0].name.endByte);
    CHECK(std::string(slice0) == f);
    CHECK(siteSet->mutation.sites[1].kind == angel_lsp::analysis::MutationSiteKind::NamedTarget);
    const std::string_view slice1 = angel_lsp::utils::SliceSource(source, siteSet->mutation.sites[1].name.startByte,
                                                                  siteSet->mutation.sites[1].name.endByte);
    CHECK(std::string(slice1) == f);
    CHECK(siteSet->mutation.sites[2].kind == angel_lsp::analysis::MutationSiteKind::ThisMember);
    const std::string_view slice2 = angel_lsp::utils::SliceSource(source, siteSet->mutation.sites[2].name.startByte,
                                                                  siteSet->mutation.sites[2].name.endByte);
    CHECK(std::string(slice2) == f);
    CHECK(siteSet->mutation.sites[3].kind == angel_lsp::analysis::MutationSiteKind::ThisCall);
    const std::string_view slice3 = angel_lsp::utils::SliceSource(source, siteSet->mutation.sites[3].name.startByte,
                                                                  siteSet->mutation.sites[3].name.endByte);
    CHECK(std::string(slice3) == get);

    bool foundFieldF = false;
    for (const auto& fieldRange : siteSet->mutation.classFieldNames)
    {
        const std::string_view fieldSlice =
            angel_lsp::utils::SliceSource(source, fieldRange.startByte, fieldRange.endByte);
        if (std::string(fieldSlice) == f)
        {
            foundFieldF = true;
            break;
        }
    }
    CHECK(foundFieldF);

    const auto getColLine5 = static_cast<uint32_t>(source.find(get, line5Start) - line5Start);
    const auto callSite = angel_lsp::analysis::FindConstCallSite(fx.index, 5, getColLine5);
    const std::string_view methodSlice =
        angel_lsp::utils::SliceSource(source, callSite.methodName.startByte, callSite.methodName.endByte);
    CHECK(std::string(methodSlice) == get);
    const std::string_view receiverSlice =
        angel_lsp::utils::SliceSource(source, callSite.receiver.startByte, callSite.receiver.endByte);
    CHECK(std::string(receiverSlice) == h);

    const auto paramListEnd = angel_lsp::analysis::FindMethodParameterListEnd(fx.index, 2, getColLine2);
    REQUIRE(paramListEnd.has_value());
    CHECK(paramListEnd->line == siteGet->parameterListEnd.line);
    CHECK(paramListEnd->character == siteGet->parameterListEnd.character);

    const std::string fn2 = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string cls2 = angel_lsp::test::GenerateRandomSymbolName("cls");
    const std::string v = angel_lsp::test::GenerateRandomSymbolName("v");
    const std::string handleSource = "void " + fn2 + "() { " + cls2 + "@ " + v + " = null; }";
    angel_lsp::parser::TreePtr handleTree = angel_lsp::parser::MakeTreePtr(parser.Parse(handleSource));
    angel_lsp::test::NodeIndexFixture handleFx(handleTree.get());

    const auto vCol = static_cast<uint32_t>(handleSource.find(v));
    const auto cls2Col = static_cast<uint32_t>(handleSource.find(cls2));
    const auto handleSite = angel_lsp::analysis::FindConstHandleSite(handleFx.index, 0, vCol);
    REQUIRE(handleSite.has_value());
    const std::string_view typeSlice =
        angel_lsp::utils::SliceSource(handleSource, handleSite->typeText.startByte, handleSite->typeText.endByte);
    CHECK(std::string(typeSlice.substr(0, cls2.size())) == cls2);
    CHECK(handleSite->typeStart.character == cls2Col);

    angel_lsp::test::NodeIndexFixture emptyFx(nullptr);
    CHECK_FALSE(angel_lsp::analysis::FindIntentionalConstSite(emptyFx.index, source, 2, getColLine2).has_value());
    CHECK_FALSE(angel_lsp::analysis::FindMethodParameterListEnd(emptyFx.index, 2, getColLine2).has_value());
    CHECK_FALSE(angel_lsp::analysis::FindConstHandleSite(emptyFx.index, 0, vCol).has_value());
    const auto emptyCallSite = angel_lsp::analysis::FindConstCallSite(emptyFx.index, 5, getColLine5);
    CHECK(emptyCallSite.methodName.Empty());
}
