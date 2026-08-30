#include <doctest/doctest.h>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

// =====================================================================================
// `{ ... }` is not a general-purpose initializer.
//
// It is valid only where the target type registered a list factory, which the primitives never do
// and never can. Every expectation below is the real compiler's own answer, taken from asharness:
//
//   int x = {5};                        -> Initialization lists cannot be used with 'int'
//   array<int> a = {1, 2, {3}, 4};      -> Initialization lists cannot be used with 'int'
//   int[] a = {1, {2}};                 -> Initialization lists cannot be used with 'int'
//   array<array<int>> g = {{1,2},{3,4}} -> accepted
//   array<array<array<int>>> d = {{{1}},{{2}}} -> accepted
//
// The last two are the reason this cannot be "a nested list is an error": nesting is exactly right
// when the element type is itself an array. The rule is about what the innermost element type is,
// which is why the check descends one element type per level.
//
// Two things are deliberately NOT inferred, and asharness is the reason for both:
//
//  1. Only primitives are reported. `string s = {1};` is an error too, but a host may register a
//     list factory for its own string type, and an unproven error is not worth a false positive.
//     A primitive is the one case where there is nothing left to prove.
//
//  2. Which templates behave like `array<T>` is configuration, not a guess. AS-Harness declares
//     `optional<T>` in the same shape as `array<T>` - one type parameter, nothing to tell them
//     apart - and the compiler answers `optional<int> o = {1};` with "Initialization lists cannot
//     be used with 'optional<int>'". `dictionary` takes a list of `{key, value}` pairs, so
//     `dictionary d = {1, 2};` is rejected as well. The list factory lives in C++ and no predefined
//     stub can express it, so TypeConfig is asked instead - see the custom-template case below.
// =====================================================================================

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    constexpr const char *k_code = "as-err-initializer-list-not-supported";
    constexpr const char *k_expectedCode = "as-err-initializer-list-expected";

    /** @brief The array template, declared the way a predefined stub would declare it. */
    const std::string k_arrayStub =
        "class array<T>\n"
        "{\n"
        "    array();\n"
        "    array(uint length);\n"
        "    uint length() const;\n"
        "}\n";

    /**
     * @brief Runs the analyzer over `k_arrayStub + body`.
     * @param arrayLike Templates to configure as element-wise, beyond the default `array`. Pass
     *                  something else to model a host that registered its own list factory.
     * @param arrayTypeName The engine's default array type; "" models one that names none.
     */
    std::vector<Diagnostic> Diagnose(const std::string &body,
                                     const std::unordered_set<std::string> &arrayLike = {},
                                     const std::string &arrayTypeName = "array")
    {
        const std::string code = k_arrayStub + body;

        AngelScriptParser parser;
        SymbolCollector symbolCollector(nullptr);
        LocalScopeCollector scopeCollector(nullptr);
        SymbolTable symbolTable;
        angel_lsp::i18n::I18n i18n;
        const std::string uri = "file:///initializer.as";

        angel_lsp::config::TypeConfig types;
        types.arrayTypeName = arrayTypeName;
        types.arrayLikeTemplates = arrayLike;

        TSTree *tree = parser.Parse(code);
        symbolCollector.CollectSymbols(uri, code, parser, symbolTable);

        SemanticAnalysisRequest request{ symbolTable, uri, "", &i18n };
        request.typeConfig = &types;
        request.scopeRoot = scopeCollector.CollectScopes(code, parser);
        request.sourceCode = code;
        request.tree = tree;

        SemanticAnalyzer analyzer(nullptr);
        std::vector<Diagnostic> result;
        for (auto &diagnostic : analyzer.Analyze(request))
        {
            if (diagnostic.code == k_code || diagnostic.code == k_expectedCode)
            {
                result.push_back(std::move(diagnostic));
            }
        }

        if (tree)
        {
            ts_tree_delete(tree);
        }
        return result;
    }

    /**
     * @brief Diagnose without the two-code filter, for rules that emit something else.
     *
     * The element-type check reports as-err-no-implicit-conversion, which belongs to the conversion
     * pass whose judgement it borrows - so it does not pass through the filter above.
     */
    std::vector<Diagnostic> DiagnoseAll(const std::string &body)
    {
        const std::string code = k_arrayStub + body;

        AngelScriptParser parser;
        SymbolCollector symbolCollector(nullptr);
        LocalScopeCollector scopeCollector(nullptr);
        SymbolTable symbolTable;
        angel_lsp::i18n::I18n i18n;
        const std::string uri = "file:///initializer.as";

        angel_lsp::config::TypeConfig types;

        TSTree *tree = parser.Parse(code);
        symbolCollector.CollectSymbols(uri, code, parser, symbolTable);

        SemanticAnalysisRequest request{ symbolTable, uri, "", &i18n };
        request.typeConfig = &types;
        request.scopeRoot = scopeCollector.CollectScopes(code, parser);
        request.sourceCode = code;
        request.tree = tree;

        SemanticAnalyzer analyzer(nullptr);
        auto result = analyzer.Analyze(request);

        if (tree)
        {
            ts_tree_delete(tree);
        }
        return result;
    }

    bool HasAnyCode(const std::vector<Diagnostic> &diagnostics, const std::string &code)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&](const Diagnostic &d) { return d.code == code; });
    }

    bool Names(const std::vector<Diagnostic> &diagnostics, const std::string &typeName)
    {
        const std::string quoted = "'" + typeName + "'";
        return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic &d)
                           { return d.message.find(quoted) != std::string::npos; });
    }
}

TEST_CASE("InitializerList - A primitive cannot be built from a list")
{
    const auto diagnostics = Diagnose("void main() { int x = {5}; }\n");
    REQUIRE(diagnostics.size() == 1);
    CHECK(Names(diagnostics, "int"));
}

TEST_CASE("InitializerList - A stray nested list inside an array of primitives")
{
    const auto diagnostics = Diagnose("void main() { array<int> a = {1, 2, {3}, 4}; }\n");
    REQUIRE(diagnostics.size() == 1);
    CHECK(Names(diagnostics, "int"));
}

TEST_CASE("InitializerList - The bracket spelling of an array behaves the same")
{
    // `T[]` needs no configuration: the language spells an array that way whatever type the engine
    // registered as its default array, so the suffix alone settles it. Hence the empty name here.
    const auto diagnostics = Diagnose("void main() { int[] a = {1, {2}}; }\n", {}, /*arrayTypeName=*/"");
    REQUIRE(diagnostics.size() == 1);
    CHECK(Names(diagnostics, "int"));
}

TEST_CASE("InitializerList - An empty nested list is still a list")
{
    // `{}` carries no elements to be wrong about, but it is still a list where an `int` was wanted,
    // and the compiler rejects it for that reason alone.
    const auto diagnostics = Diagnose("void main() { array<int> a = {1, {}, 2}; }\n");
    CHECK(diagnostics.size() == 1);
}

TEST_CASE("InitializerList - Nesting is correct when the element type is an array")
{
    CHECK(Diagnose("void main() { array<array<int>> g = {{1, 2}, {3, 4}}; }\n").empty());
    CHECK(Diagnose("void main() { array<array<array<int>>> d = {{{1}}, {{2}}}; }\n").empty());
    CHECK(Diagnose("void main() { int[][] g = {{1, 2}, {3, 4}}; }\n").empty());
}

TEST_CASE("InitializerList - A flat list of primitives is correct")
{
    CHECK(Diagnose("void main() { array<int> a = {1, 2, 3, 4}; }\n").empty());
    CHECK(Diagnose("void main() { array<int> a; }\n").empty());
    CHECK(Diagnose("void main() { array<int> a(10); }\n").empty());
}

TEST_CASE("InitializerList - Depth beyond the declared type is where the error is reported")
{
    // Two levels of array, three levels of braces. The mismatch is at the innermost one, and the
    // compiler names `int` rather than the type that was declared.
    const auto diagnostics = Diagnose("void main() { array<array<int>> g = {{{1}}}; }\n");
    REQUIRE(diagnostics.size() == 1);
    CHECK(Names(diagnostics, "int"));
}

TEST_CASE("InitializerList - Nothing is said about a type whose list pattern is unknown")
{
    // A host template this analyzer has never seen may accept a list of any shape it likes, and a
    // class may have registered a list factory in C++ where no stub records it. Both stay silent.
    CHECK(Diagnose("class Config {}\nvoid main() { Config c = {1, {2}}; }\n").empty());
    CHECK(Diagnose("void main() { dictionary d = {{'a', 1}}; }\n").empty());
}

// =====================================================================================
// The array template is named by configuration, not by this file.
// =====================================================================================

TEST_CASE("InitializerList - A host's own element-wise template is honoured once configured")
{
    const std::string body =
        "class vector<T> { uint size() const; }\n"
        "void main() { vector<int> v = {1, {2}, 3}; }\n";

    // Undeclared, the rule cannot know whether `vector<T>` takes a list at all - `optional<T>` is
    // declared identically and takes none - so it stays silent.
    CHECK(Diagnose(body).empty());

    // Named by `--array-like-type=vector`, it reads exactly as `array<T>` does.
    const auto diagnostics = Diagnose(body, { "vector" });
    REQUIRE(diagnostics.size() == 1);
    CHECK(Names(diagnostics, "int"));
}

TEST_CASE("InitializerList - A renamed array type is followed and the default is not assumed")
{
    // A host whose default array is registered under another name. Nothing may fall back to the
    // spelling `array` on its own: here that name belongs to no such type.
    const std::string body = "class vec<T> {}\nvoid main() { vec<int> v = {1, {2}}; }\n";

    const auto diagnostics = Diagnose(body, {}, /*arrayTypeName=*/"vec");
    REQUIRE(diagnostics.size() == 1);
    CHECK(Names(diagnostics, "int"));

    // ...and with no array type configured at all, `Name<T>` says nothing.
    CHECK(Diagnose(body, {}, /*arrayTypeName=*/"").empty());
}

TEST_CASE("InitializerList - A multi-argument template is not element-wise")
{
    // `map<K, V>` has no single element type, so it is not one of these however it was configured.
    const std::string body =
        "class map<K, V> {}\n"
        "void main() { map<string, int> m = {{'a', {1}}}; }\n";
    CHECK(Diagnose(body, { "map" }).empty());
}

// =====================================================================================
// Type decorations.
//
// `const`, `&` and `@` are language syntax and are stripped before the shape is read; they do not
// change which values a type accepts. The boundary matters: a class whose name merely ends in
// `const` is not a qualified type.
// =====================================================================================

TEST_CASE("InitializerList - Decorations are stripped without truncating names")
{
    CHECK(Diagnose("void main() { const int x = {5}; }\n").size() == 1);
    CHECK(Diagnose("void main() { const array<int> a = {1, {2}}; }\n").size() == 1);

    // `Wconst` is a class name, not `W` with a qualifier. Truncating it would make the rule look up
    // a type that was never written.
    CHECK(Diagnose("class Wconst {}\nvoid main() { Wconst w = {1, {2}}; }\n").empty());
}

// =====================================================================================
// The list pattern comes from the stub, not from this file.
//
// AngelScript's list factory carries its pattern in the registration string itself:
//
//     asBEHAVE_LIST_FACTORY, "array<T>@ f(int&in type, int&in list) {repeat T}"
//     asBEHAVE_LIST_FACTORY, "dictionary @f(int &in) {repeat {string, ?}}"
//
// A predefined stub carries the same text as a `@listpattern` tag, so a host that registers its own
// list factory gets the rule without this server knowing anything about its types. Every
// expectation below is asharness's answer for the corresponding standard add-on.
// =====================================================================================

namespace
{
    /** @brief A stub declaring one class with the given pattern, plus a body that uses it. */
    std::string WithPattern(const std::string &declaration, const std::string &pattern,
                            const std::string &body)
    {
        return "/// @listpattern " + pattern + "\n" + declaration + "\n" + body;
    }
}

TEST_CASE("ListPattern - A tagged template needs no configuration at all")
{
    // No --array-like-type, no arrayTypeName match: the stub alone carries it.
    const std::string code = WithPattern("class vector<T> { uint size() const; }", "{repeat T}",
                                         "void main() { vector<int> v = {1, {2}, 3}; }\n");

    const auto diagnostics = Diagnose(code, {}, /*arrayTypeName=*/"");
    REQUIRE(diagnostics.size() == 1);
    CHECK(Names(diagnostics, "int"));
}

TEST_CASE("ListPattern - An untagged template of the same shape stays silent")
{
    // `optional<T>` is declared exactly like `array<T>` and registers no list factory; the compiler
    // answers `optional<int> o = {1};` with "Initialization lists cannot be used with 'optional<int>'".
    // Without a tag this server cannot tell the two apart, and says nothing rather than guessing.
    const std::string code =
        "class optional<T> { bool has_value() const; }\n"
        "void main() { optional<int> o = {1, {2}}; }\n";

    CHECK(Diagnose(code, {}, /*arrayTypeName=*/"").empty());
}

TEST_CASE("ListPattern - A dictionary's pair pattern is honoured in both directions")
{
    const std::string declaration = "class dict { bool exists(const string &in) const; }";
    const std::string pattern = "{repeat {string, ?}}";

    // `{{'a', 1}}` matches: one repetition of a two-item group.
    CHECK(Diagnose(WithPattern(declaration, pattern,
                               "void main() { dict d = {{'a', 1}}; }\n")).empty());

    // `{1, 2}` does not: the pattern wants a nested list per element, and the compiler says
    // "Expected a list enclosed by { } to match pattern".
    const auto flat = Diagnose(WithPattern(declaration, pattern,
                                           "void main() { dict d = {1, 2}; }\n"));
    REQUIRE(flat.size() == 2);
    CHECK(flat[0].code == "as-err-initializer-list-expected");

    // `?` takes a value of any type, not a list of any shape. The compiler rejects a list there
    // with "Initialization lists cannot be used with '?'".
    const auto nested = Diagnose(WithPattern(declaration, pattern,
                                             "void main() { dict d = {{'a', {1}}}; }\n"));
    REQUIRE(nested.size() == 1);
    CHECK(Names(nested, "?"));
}

TEST_CASE("ListPattern - A tagged element type makes a nested list correct")
{
    // `array<dict> a = {{{'a', 1}}};` is accepted by the compiler: the outer list repeats `dict`,
    // and `dict` in turn accepts the inner one. Nesting is only wrong where the element type
    // accepts no list, which is what makes this the same rule as the primitive case.
    const std::string code =
        "/// @listpattern {repeat {string, ?}}\n"
        "class dict {}\n"
        "void main() { array<dict> a = {{{'a', 1}}}; }\n";
    CHECK(Diagnose(code).empty());
}

TEST_CASE("ListPattern - A malformed tag is ignored rather than reported")
{
    // A stub this server cannot read is not the stub author's error to see here. It falls back to
    // saying nothing, exactly as an absent tag does.
    const std::string code = WithPattern("class thing {}", "{repeat",
                                         "void main() { thing t = {1, {2}}; }\n");
    CHECK(Diagnose(code, {}, /*arrayTypeName=*/"").empty());
}

// =====================================================================================
// An element's type, not just the list's shape.
//
// The conversion pass skips initializer lists outright, so nothing judged what was inside one and
// `array<int> a = {"x"}` was silent. The compiler's own answer:
//
//     ERROR (1, 32): Can't implicitly convert from 'const string' to 'int&'.
//
// tests/parity/doc_r10_initlist_element_type.as. The judgement is borrowed from
// CanConvertImplicitly rather than reimplemented here, which also borrows its silence.
// =====================================================================================

TEST_CASE("InitializerList - an element that cannot reach the element type is reported")
{
    const auto diagnostics = DiagnoseAll("void main() { array<int> a = {\"x\"}; }\n");

    CHECK(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("InitializerList - an element that can reach it stays silent")
{
    const auto diagnostics = DiagnoseAll("void main() { array<int> a = {1, 2, 3}; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("InitializerList - a widening element is not a mismatch")
{
    // int -> double is a conversion the compiler makes without comment, and so does this.
    const auto diagnostics = DiagnoseAll("void main() { array<double> a = {1, 2}; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("InitializerList - an element of unknown type is passed over")
{
    // `Whatever()` resolves to nothing this analyzer can read, and not knowing is never a reason to
    // report. The borrowed judgement carries that asymmetry.
    const auto diagnostics = DiagnoseAll("void main() { array<int> a = {Whatever()}; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
}

// =====================================================================================
// When nothing said what the list should look like.
//
// A list factory is registered in C++ and no stub can express it, so a type with no
// `/// @listpattern` tag has its list left entirely unchecked - shape and contents both. Silence is
// the right verdict and a useless explanation: the user sees a list going unchecked with no way to
// know that one doc tag would fix it. A Hint says so, at the declaration, and only where it can be
// acted on - an engine-registered type has no declaration to tag.
// =====================================================================================

TEST_CASE("InitializerList - a declared type with no list pattern is hinted, not reported")
{
    const auto diagnostics = DiagnoseAll(
        "class Config { }" + std::string(1, char(10)) +
        "void main() { Config c = {1, 2}; }" + std::string(1, char(10)));

    CHECK(HasAnyCode(diagnostics, "as-hint-list-pattern-unknown"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
}

TEST_CASE("InitializerList - a type that does declare a pattern is not hinted")
{
    // k_arrayStub's array<T> is the engine's array, so the pattern resolves and the list is
    // actually checked. Nothing to suggest.
    const auto diagnostics = DiagnoseAll("void main() { array<int> a = {1, 2}; }" + std::string(1, char(10)));

    CHECK_FALSE(HasAnyCode(diagnostics, "as-hint-list-pattern-unknown"));
}

TEST_CASE("InitializerList - an invisible type is not hinted either")
{
    // `HostThing` resolves to no declaration, so there is nowhere to put the tag and nothing
    // useful to say. This is the same visibility test the rest of the pass makes, used to decide
    // whether to suggest rather than whether to report.
    const auto diagnostics = DiagnoseAll("void main() { HostThing h = {1, 2}; }" + std::string(1, char(10)));

    CHECK_FALSE(HasAnyCode(diagnostics, "as-hint-list-pattern-unknown"));
}

TEST_CASE("InitializerList - a primitive is reported, not hinted")
{
    // A primitive is the one family that can never have a list factory, so its silence in a stub
    // proves something. That stays an error and gains no suggestion.
    const auto diagnostics = DiagnoseAll("void main() { int x = {5}; }" + std::string(1, char(10)));

    CHECK(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-hint-list-pattern-unknown"));
}

// =====================================================================================
// The other three positions a list may be written in.
//
// The grammar allows `initializer_list` under exactly six parents - a declarator, an argument list,
// an assignment's right-hand side, a return, another list, and the `type = {...}` anonymous object -
// and only the first was ever visited. The compiler infers the target type in each of the others
// and compiles the list against it, so all four of these are errors it reports and this analyzer
// used to pass over in silence:
//
//   void Take(array<int> v); Take({"x"});  Can't implicitly convert from 'const string' to 'int&'
//   array<int> a;  a = {"x"};              Can't implicitly convert from 'const string' to 'int&'
//   array<int> Make() { return {1,"x"}; }  Can't implicitly convert from 'const string' to 'int&'
//   Take({1, {2}});                        Initialization lists cannot be used with 'int'
//
// tests/parity/doc_r18 through doc_r20, with doc_p18 holding every one of them written correctly.
// =====================================================================================

TEST_CASE("InitializerList - a list argument is judged against the parameter")
{
    const auto diagnostics = DiagnoseAll(
        "void Take(array<int> values) {}\n"
        "void main() { Take({\"x\"}); }\n");

    CHECK(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("InitializerList - a nested list in an argument is judged too")
{
    const auto diagnostics = DiagnoseAll(
        "void Take(array<int> values) {}\n"
        "void main() { Take({1, {2}}); }\n");

    CHECK(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
    CHECK(Names(diagnostics, "int"));
}

TEST_CASE("InitializerList - a correct list argument stays silent")
{
    const auto diagnostics = DiagnoseAll(
        "void Take(array<int> values) {}\n"
        "void main() { Take({1, 2}); }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
}

TEST_CASE("InitializerList - an overloaded call leaves its list alone")
{
    // Which parameter the list is built against is the overload's answer, and with two candidates
    // the compiler does not give one either: `Multiple matching signatures to 'Take({...})'`. A
    // verdict about the list's contents here would be a guess about which overload was meant.
    const auto diagnostics = DiagnoseAll(
        "void Take(array<int> values) {}\n"
        "void Take(array<string> values) {}\n"
        "void main() { Take({\"x\"}); }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
}

TEST_CASE("InitializerList - a list assigned to a variable is judged against it")
{
    const auto diagnostics = DiagnoseAll(
        "void main() { array<int> a; a = {\"x\"}; }\n");

    CHECK(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("InitializerList - a correct assignment stays silent")
{
    const auto diagnostics = DiagnoseAll(
        "void main() { array<int> a; a = {1, 2}; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
}

TEST_CASE("InitializerList - a compound assignment says nothing about its list")
{
    // `a += {1};` is rejected as "Illegal operation on 'int[]&'" - a verdict about the operator,
    // not about the list's shape. Blaming the list would name the wrong thing.
    const auto diagnostics = DiagnoseAll(
        "void main() { array<int> a; a += {1}; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("InitializerList - a returned list is judged against the declared return type")
{
    const auto diagnostics = DiagnoseAll(
        "array<int> Make() { return {1, \"x\"}; }\n");

    CHECK(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
}

TEST_CASE("InitializerList - a correct returned list stays silent")
{
    const auto diagnostics = DiagnoseAll(
        "array<int> Make() { return {1, 2}; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
}

TEST_CASE("InitializerList - a list returned from a lambda stays silent")
{
    // A lambda returns into whichever funcdef it is being assigned to, which is not written at the
    // list. The enclosing function's return type is the wrong answer, so the walk stops.
    const auto diagnostics = DiagnoseAll(
        "funcdef array<int>@ Factory();\n"
        "void main() { Factory@ f = function() { return {1, \"x\"}; }; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
}

TEST_CASE("InitializerList - an anonymous object carries its own target type")
{
    // `array<int> = {...}` writes the type at the list, so nothing has to be inferred to check it.
    const auto diagnostics = DiagnoseAll(
        "void Take(array<int> values) {}\n"
        "void main() { Take(array<int> = {1, {2}}); }\n");

    CHECK(HasAnyCode(diagnostics, "as-err-initializer-list-not-supported"));
    CHECK(Names(diagnostics, "int"));
}

// =====================================================================================
// How many values a pattern wants.
//
// A group with no `repeat` in it is an exact count, and the compiler says so in both directions.
// Against `dictionary`'s `{repeat {string, ?}}`, whose inner group is a fixed pair:
//
//   dictionary d = {{'a'}};       Not enough values to match pattern   (doc_r21)
//   dictionary d = {{'a', 1, 2}}; Too many values to match pattern     (doc_r22)
//
// A `repeat` has no count to check: it consumes every element from its position onward and is
// satisfied by none at all, which is why `array<int> a = {};` compiles.
//
// The count comes from the separators, not from the nodes: an omitted element produces no node and
// the compiler still counts it, which `dictionary d = {{'a',}};` proves by compiling.
// =====================================================================================

namespace
{
    /** @brief The dictionary pattern, declared the way the SDK stub declares it. */
    const std::string k_dictStub =
        "/// @listpattern {repeat {string, ?}}\n"
        "class dict {}\n";
}

TEST_CASE("ListPattern - a fixed group wants exactly its own number of values")
{
    const auto tooFew = DiagnoseAll(k_dictStub + "void main() { dict d = {{'a'}}; }\n");
    CHECK(HasAnyCode(tooFew, "as-err-initializer-list-too-few"));

    const auto tooMany = DiagnoseAll(k_dictStub + "void main() { dict d = {{'a', 1, 2}}; }\n");
    CHECK(HasAnyCode(tooMany, "as-err-initializer-list-too-many"));

    const auto exact = DiagnoseAll(k_dictStub + "void main() { dict d = {{'a', 1}}; }\n");
    CHECK_FALSE(HasAnyCode(exact, "as-err-initializer-list-too-few"));
    CHECK_FALSE(HasAnyCode(exact, "as-err-initializer-list-too-many"));
}

TEST_CASE("ListPattern - a repeat has no count to check")
{
    const auto diagnostics = DiagnoseAll(
        "void main() { array<int> none = {}; array<int> many = {1, 2, 3, 4, 5}; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-too-few"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-too-many"));
}

TEST_CASE("ListPattern - a fixed pattern at the top level is counted too")
{
    // `complex`'s registration is `{float, float}` - the SDK stub in tests/fixtures carries it.
    const std::string stub = "/// @listpattern {float, float}\nclass complex {}\n";

    CHECK(HasAnyCode(DiagnoseAll(stub + "void main() { complex c = {1, 2, 3}; }\n"),
                     "as-err-initializer-list-too-many"));
    CHECK(HasAnyCode(DiagnoseAll(stub + "void main() { complex c = {1}; }\n"),
                     "as-err-initializer-list-too-few"));
    CHECK_FALSE(HasAnyCode(DiagnoseAll(stub + "void main() { complex c = {1, 2}; }\n"),
                           "as-err-initializer-list-too-many"));
}

TEST_CASE("ListPattern - an omitted element counts as a value")
{
    // The guard for counting separators rather than nodes, and it could not be written until the
    // grammar pin moved: before aa14847 a hole made the whole declaration an ERROR node, so there
    // was no list to count. `dictionary d = {{'a',}};` compiles - the hole fills the pattern's
    // second slot with the type's default - and counting the two child nodes of `{'a',}` as one
    // value would have reported "Not enough values" on code the compiler accepts.
    const auto diagnostics = DiagnoseAll(k_dictStub + "void main() { dict d = {{'a',}}; }\n");

    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-too-few"));
    CHECK_FALSE(HasAnyCode(diagnostics, "as-err-initializer-list-too-many"));
}
