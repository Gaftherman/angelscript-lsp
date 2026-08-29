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
