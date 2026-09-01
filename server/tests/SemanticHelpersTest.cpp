#include <doctest/doctest.h>

#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

#include <string>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

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

    const std::string code =
        "void Foo(int a) {}\n"
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
