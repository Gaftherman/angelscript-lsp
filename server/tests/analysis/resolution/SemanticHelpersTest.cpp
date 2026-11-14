#include "analysis/resolution/SemanticHelpers.h"

#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <string>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_SUITE("analysis/resolution/SemanticHelpers")
{
    // =====================================================================================
    // A type position naming a function, and the funcdef that would make it legal.
    //
    // `void Foo(int) {}` then `Foo@ h = @Foo;` is rejected - "Identifier 'Foo' is not a data type",
    // verified against angelscript_oracle - because a function handle needs a funcdef to name its
    // signature. The signature is sitting on the function, so the fix can write it.
    //
    // Opt-in and a Hint: the name could equally belong to a host type this analyzer cannot see.
    // =====================================================================================

    TEST_CASE("SemanticHelpers - NamesAFunctionNotAType needs both halves")
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        SymbolTable table;

        const std::string code = "void Foo(int a) {}\n"
                                 "class Bar {}\n"
                                 "funcdef void Baz(int);\n"
                                 "void Baz(int a) {}\n"
                                 "class Container { void Method() {} }\n";

        collector.CollectSymbols("file:///t.as", code, parser, table);

        // A plain global function: yes.
        CHECK(NamesAFunctionNotAType("Foo", table));

        // A class: no, it is a type.
        CHECK_FALSE(NamesAFunctionNotAType("Bar", table));

        // A name that is BOTH a funcdef and a function is already usable as a type, so there is
        // nothing to suggest.
        CHECK_FALSE(NamesAFunctionNotAType("Baz", table));

        // A method cannot be written bare in a type position, so it says nothing about the intent.
        CHECK_FALSE(NamesAFunctionNotAType("Method", table));

        // A name nothing declares is an unresolved type, assumed engine-registered. Not this rule's
        // business, and reporting it would contradict the analyzer's central policy.
        CHECK_FALSE(NamesAFunctionNotAType("CBaseEntity", table));

        CHECK_FALSE(NamesAFunctionNotAType("", table));
    }

    TEST_CASE("SemanticHelpers - IsDestructorDeclaration and IsDestructorSymbol inspect first-pass flag")
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        SymbolTable table;

        const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Entity");
        const std::string spacedCls = angel_lsp::test::GenerateRandomSymbolName("Widget");
        const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("Tick");
        const std::string freeName = angel_lsp::test::GenerateRandomSymbolName("Run");

        const std::string code = "class " + clsName +
                                 " {\n"
                                 "    ~" +
                                 clsName +
                                 "() {}\n"
                                 "    void " +
                                 methodName +
                                 "() {}\n"
                                 "}\n"
                                 "class " +
                                 spacedCls +
                                 " {\n"
                                 "    ~\t " +
                                 spacedCls +
                                 "() {}\n"
                                 "}\n"
                                 "void " +
                                 freeName + "() {}\n";

        collector.CollectSymbols("file:///dtor_helpers.as", code, parser, table);

        const auto directSyms = table.FindSymbols(clsName + "::" + clsName);
        REQUIRE(directSyms.size() == 1);
        CHECK(IsDestructorDeclaration(directSyms[0]));
        CHECK(IsDestructorSymbol(directSyms[0]));

        const auto spacedSyms = table.FindSymbols(spacedCls + "::" + spacedCls);
        REQUIRE(spacedSyms.size() == 1);
        CHECK(IsDestructorDeclaration(spacedSyms[0]));
        CHECK(IsDestructorSymbol(spacedSyms[0]));

        const auto methodSyms = table.FindSymbols(clsName + "::" + methodName);
        REQUIRE(methodSyms.size() == 1);
        CHECK_FALSE(IsDestructorDeclaration(methodSyms[0]));
        CHECK_FALSE(IsDestructorSymbol(methodSyms[0]));

        const auto freeSyms = table.FindSymbols(freeName);
        REQUIRE(freeSyms.size() == 1);
        CHECK_FALSE(IsDestructorDeclaration(freeSyms[0]));
        CHECK_FALSE(IsDestructorSymbol(freeSyms[0]));

        const auto classSyms = table.FindSymbols(clsName);
        REQUIRE(classSyms.size() == 1);
        CHECK_FALSE(IsDestructorDeclaration(classSyms[0]));
        CHECK_FALSE(IsDestructorSymbol(classSyms[0]));
    }
}
