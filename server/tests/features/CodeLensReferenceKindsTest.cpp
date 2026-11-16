#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/SymbolCollector.h"
#include "config/ServerConfig.h"
#include "features/code_lens/CodeLensHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
struct KindLensFixture
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;
    std::string uri = "file:///test.as";
    std::string sourceCode;

    explicit KindLensFixture(std::string code) : sourceCode(std::move(code))
    {
        AddDocument(uri, sourceCode);
    }

    void AddDocument(const std::string& fileUri, const std::string& code)
    {
        symbolCollector.CollectSymbols(fileUri, code, parser, symbolTable);
        auto scopeRoot = scopeCollector.CollectScopes(code, parser);
        if (scopeRoot)
        {
            scopeIndex.SetScopeTree(fileUri, std::move(scopeRoot));
        }
    }

    std::optional<std::vector<lsp::CodeLens>> GetLenses(const config::ServerConfig* config = nullptr) const
    {
        CodeLensRequest req{uri,     sourceCode, symbolTable, scopeIndex, nullptr, ".as.predefined",
                            nullptr, nullptr,    config};
        return GetCodeLenses(req);
    }
};

uint32_t LineOf(std::string_view code, std::string_view needle)
{
    const size_t pos = code.find(needle);
    REQUIRE(pos != std::string_view::npos);
    return static_cast<uint32_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
}

std::optional<std::string> LensTitleAt(const std::vector<lsp::CodeLens>& lenses, uint32_t line)
{
    for (const auto& lens : lenses)
    {
        if (lens.range.start.line == line && lens.command.has_value())
        {
            return lens.command->title;
        }
    }
    return std::nullopt;
}
} // namespace

TEST_SUITE("CodeLens - Reference Kinds")
{
    TEST_CASE("CodeLens - Global and nested funcdef count only declared-type uses")
    {
        const std::string globalFd = angel_lsp::test::GenerateRandomSymbolName("GlobalFn");
        const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("UtilNs");
        const std::string nsFd = angel_lsp::test::GenerateRandomSymbolName("NsFn");
        const std::string hostCls = angel_lsp::test::GenerateRandomSymbolName("HostCls");
        const std::string clsFd = angel_lsp::test::GenerateRandomSymbolName("ClsFn");
        const std::string tmplCls = angel_lsp::test::GenerateRandomSymbolName("Box");
        const std::string tmplFd = angel_lsp::test::GenerateRandomSymbolName("BoxFn");
        const std::string targetFn = angel_lsp::test::GenerateRandomSymbolName("TargetFn");

        const std::string code = "funcdef bool " + globalFd +
                                 "(int a, int b);\n"
                                 "void " +
                                 targetFn +
                                 "() {}\n"
                                 "namespace " +
                                 nsName +
                                 " {\n"
                                 "    funcdef void " +
                                 nsFd +
                                 "();\n"
                                 "}\n"
                                 "class " +
                                 hostCls +
                                 " {\n"
                                 "    funcdef void " +
                                 clsFd +
                                 "();\n"
                                 "    " +
                                 clsFd +
                                 "@ innerField;\n"
                                 "}\n"
                                 "class " +
                                 tmplCls +
                                 "<T> {\n"
                                 "    funcdef bool " +
                                 tmplFd +
                                 "(const T& in a, const T& in b);\n"
                                 "}\n"
                                 "class Container<U> {}\n" +
                                 globalFd + "@ gCb;\n" + nsName + "::" + nsFd + "@ gNsCb;\n" + hostCls + "::" + clsFd +
                                 "@ gClsCb;\n" + tmplCls + "<int>::" + tmplFd +
                                 "@ gTmplCb;\n"
                                 "Container<" +
                                 globalFd + "@> gWrap;\n" + globalFd + "@ UseAll(" + globalFd +
                                 "@ p) {\n"
                                 "    " +
                                 globalFd +
                                 "@ localVar;\n"
                                 "    auto c = cast<" +
                                 globalFd +
                                 ">(p);\n"
                                 "    " +
                                 globalFd + " = " + targetFn +
                                 ";\n"
                                 "    return p;\n"
                                 "}\n";

        KindLensFixture fixture(code);
        const auto lenses = fixture.GetLenses();
        REQUIRE(lenses.has_value());

        // globalFd uses: gCb, Container<globalFd@>, return type, param p, localVar, cast<globalFd> = 6
        // Non-type expression assignment `globalFd = targetFn` must NOT count.
        CHECK(LensTitleAt(*lenses, LineOf(code, "funcdef bool " + globalFd)) == "6 references");
        CHECK(LensTitleAt(*lenses, LineOf(code, "funcdef void " + nsFd)) == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(code, "funcdef void " + clsFd)) == "2 references");
        CHECK(LensTitleAt(*lenses, LineOf(code, "funcdef bool " + tmplFd)) == "1 reference");
    }

    TEST_CASE("CodeLens - Typedef and typedef-of-typedef keep counts separate from underlying type")
    {
        const std::string baseCls = angel_lsp::test::GenerateRandomSymbolName("Metric");
        const std::string aliasOne = angel_lsp::test::GenerateRandomSymbolName("AliasOne");
        const std::string aliasTwo = angel_lsp::test::GenerateRandomSymbolName("AliasTwo");

        const std::string code = "class " + baseCls +
                                 " { int v; }\n"
                                 "typedef " +
                                 baseCls + " " + aliasOne +
                                 ";\n"
                                 "typedef " +
                                 aliasOne + " " + aliasTwo +
                                 ";\n"
                                 "class Holder<T> {}\n" +
                                 baseCls + " directUse;\n" + aliasOne + " firstUse;\n" + aliasTwo + " TakeAndReturn(" +
                                 aliasTwo +
                                 " p) {\n"
                                 "    Holder<" +
                                 aliasOne +
                                 "> h;\n"
                                 "    return cast<" +
                                 aliasTwo +
                                 ">(p);\n"
                                 "}\n";

        KindLensFixture fixture(code);
        config::ServerConfig showCfg;
        showCfg.features.codeLensShowOnClassesAndInterfaces = true;
        const auto lenses = fixture.GetLenses(&showCfg);
        REQUIRE(lenses.has_value());

        // baseCls: typedef baseCls aliasOne + directUse = 2
        CHECK(LensTitleAt(*lenses, LineOf(code, "class " + baseCls)) == "2 references");
        // aliasOne: typedef aliasOne aliasTwo + firstUse + Holder<aliasOne> = 3
        CHECK(LensTitleAt(*lenses, LineOf(code, "typedef " + baseCls + " " + aliasOne)) == "3 references");
        // aliasTwo: return type + param p + cast<aliasTwo> = 3
        CHECK(LensTitleAt(*lenses, LineOf(code, "typedef " + aliasOne + " " + aliasTwo)) == "3 references");

        config::ServerConfig hideCfg;
        hideCfg.features.codeLensShowOnClassesAndInterfaces = false;
        const auto hidden = fixture.GetLenses(&hideCfg);
        REQUIRE(hidden.has_value());
        CHECK_FALSE(LensTitleAt(*hidden, LineOf(code, "typedef " + baseCls + " " + aliasOne)).has_value());
    }

    TEST_CASE("CodeLens - Operator overloads count resolved expressions and cross-file calls")
    {
        const std::string vecCls = angel_lsp::test::GenerateRandomSymbolName("Vec");
        const std::string dstCls = angel_lsp::test::GenerateRandomSymbolName("ScalarTag");

        const std::string mainCode = "class " + dstCls +
                                     " {}\n"
                                     "class " +
                                     vecCls +
                                     " {\n"
                                     "    " +
                                     vecCls + " opAdd(const " + vecCls + "& in rhs) const { return " + vecCls +
                                     "(); }\n"
                                     "    " +
                                     vecCls + " opAdd(int scalar) const { return " + vecCls +
                                     "(); }\n"
                                     "    " +
                                     vecCls + " opMul(const " + vecCls + "& in rhs) const { return " + vecCls +
                                     "(); }\n"
                                     "    bool opEquals(const " +
                                     vecCls +
                                     "& in rhs) const { return true; }\n"
                                     "    int opIndex(int idx) const { return idx; }\n"
                                     "    " +
                                     vecCls + "& opAssign(const " + vecCls +
                                     "& in rhs) { return this; }\n"
                                     "    " +
                                     vecCls + "& opAddAssign(const " + vecCls +
                                     "& in rhs) { return this; }\n"
                                     "    int opImplConv() const { return 1; }\n"
                                     "    " +
                                     dstCls + " opCast() const { return " + dstCls +
                                     "(); }\n"
                                     "    void opSub() {}\n"
                                     "}\n"
                                     "void DriveLocal() {\n"
                                     "    " +
                                     vecCls +
                                     " a;\n"
                                     "    " +
                                     vecCls +
                                     " b;\n"
                                     "    " +
                                     vecCls +
                                     " c = a + b;\n"
                                     "    " +
                                     vecCls +
                                     " d = a * b;\n"
                                     "    bool eq = (a == b);\n"
                                     "    bool neq = (a != b);\n"
                                     "    int elem = a[0];\n"
                                     "    a = b;\n"
                                     "    a += b;\n"
                                     "    int n = a;\n"
                                     "    " +
                                     dstCls + " casted = cast<" + dstCls +
                                     ">(a);\n"
                                     "}\n";

        const std::string otherCode = "void DriveRemote() {\n"
                                      "    " +
                                      vecCls +
                                      " x;\n"
                                      "    " +
                                      vecCls +
                                      " y;\n"
                                      "    " +
                                      vecCls +
                                      " z = x + y;\n"
                                      "    int k = x[3];\n"
                                      "}\n";

        KindLensFixture fixture(mainCode);
        fixture.AddDocument("file:///other.as", otherCode);

        config::ServerConfig cfg;
        cfg.features.codeLensShowZeroReferences = true;
        const auto lenses = fixture.GetLenses(&cfg);
        REQUIRE(lenses.has_value());

        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opAdd(const " + vecCls)).value_or("<none>") == "2 references");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opAdd(int scalar)")).value_or("<none>") == "0 references");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opMul(")).value_or("<none>") == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opEquals(")).value_or("<none>") == "2 references");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opIndex(")).value_or("<none>") == "2 references");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opAssign(")).value_or("<none>") == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opAddAssign(")).value_or("<none>") == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opImplConv(")).value_or("<none>") == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opCast(")).value_or("<none>") == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opSub()")).value_or("<none>") == "0 references");
    }

    TEST_CASE("CodeLens - Chained compound expressions count each operator once")
    {
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Num");
        const std::string v = angel_lsp::test::GenerateRandomSymbolName("v");
        const std::string w = angel_lsp::test::GenerateRandomSymbolName("w");
        const std::string C = cls;

        const std::string mainCode = "class " + C +
                                     " {\n"
                                     "    " +
                                     C + " opAdd(const " + C + "&in) const { return " + C +
                                     "(); }\n"
                                     "    " +
                                     C + " opMul(const " + C + "&in) const { return " + C +
                                     "(); }\n"
                                     "    " +
                                     C + " opMul_r(int) const { return " + C +
                                     "(); }\n"
                                     "    " +
                                     C + "& opAddAssign(const " + C +
                                     "&in) { return this; }\n"
                                     "    " +
                                     C +
                                     "& opPreInc() { return this; }\n"
                                     "    " +
                                     C + " opPostInc() { return " + C +
                                     "(); }\n"
                                     "}\n"
                                     "void Drive() {\n"
                                     "    " +
                                     C + " " + v +
                                     ";\n"
                                     "    " +
                                     v + " += " + v + " + " + v + " * " + v + " * " + v +
                                     ";\n"
                                     "    " +
                                     v +
                                     "++;\n"
                                     "    ++" +
                                     v +
                                     ";\n"
                                     "    " +
                                     v + " = 2 * " + v +
                                     ";\n"
                                     "    " +
                                     v + " += (" + v + " * " + v +
                                     ") *\n"
                                     "        " +
                                     v +
                                     ";\n"
                                     "    int " +
                                     w +
                                     " = 1;\n"
                                     "    " +
                                     w + " += " + w + " + " + w + " * " + w + " * " + w +
                                     ";\n"
                                     "    " +
                                     w +
                                     "++;\n"
                                     "    ++" +
                                     w +
                                     ";\n"
                                     "}\n";
        const std::string otherCode = "void Remote() {\n"
                                      "    " +
                                      C + " " + v +
                                      ";\n"
                                      "    " +
                                      v + " += " + v + " * " + v +
                                      ";\n"
                                      "}\n";

        KindLensFixture fixture(mainCode);
        fixture.AddDocument("file:///other.as", otherCode);
        config::ServerConfig cfg;
        cfg.features.codeLensShowZeroReferences = true;
        const auto lenses = fixture.GetLenses(&cfg);
        REQUIRE(lenses.has_value());

        // opMul: 2 in the chain + 2 in the parenthesized/split line + 1 remote.
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opMul(")).value_or("<none>") == "5 references");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opMul_r(")).value_or("<none>") == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opAdd(")).value_or("<none>") == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opAddAssign(")).value_or("<none>") == "3 references");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opPostInc(")).value_or("<none>") == "1 reference");
        CHECK(LensTitleAt(*lenses, LineOf(mainCode, "opPreInc(")).value_or("<none>") == "1 reference");
    }

    TEST_CASE("CodeLens - Destructors never emit CodeLens even inside namespaces")
    {
        const std::string clsTop = angel_lsp::test::GenerateRandomSymbolName("ResTop");
        const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("ResNs");
        const std::string clsNs = angel_lsp::test::GenerateRandomSymbolName("ResInner");

        const std::string code = "class " + clsTop +
                                 " {\n"
                                 "    " +
                                 clsTop +
                                 "() {}\n"
                                 "    ~" +
                                 clsTop +
                                 "() {}\n"
                                 "}\n"
                                 "namespace " +
                                 nsName +
                                 " {\n"
                                 "    class " +
                                 clsNs +
                                 " {\n"
                                 "        ~" +
                                 clsNs +
                                 "() {}\n"
                                 "    }\n"
                                 "}\n";

        KindLensFixture fixture(code);
        config::ServerConfig cfg;
        cfg.features.codeLensShowZeroReferences = true;
        const auto lenses = fixture.GetLenses(&cfg);
        REQUIRE(lenses.has_value());

        CHECK(LensTitleAt(*lenses, LineOf(code, clsTop + "() {}")) == "0 references");
        CHECK_FALSE(LensTitleAt(*lenses, LineOf(code, "~" + clsTop + "()")).has_value());
        CHECK_FALSE(LensTitleAt(*lenses, LineOf(code, "~" + clsNs + "()")).has_value());
    }
}
