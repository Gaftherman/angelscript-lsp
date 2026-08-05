#include <doctest/doctest.h>
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include "i18n/i18n.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

static bool TestSemanticCodeGroupC(const std::string &sourceCode, const std::string &expectedCode = "")
{
    std::string fileUri = "file:///test_suite_group_c.as";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto syntaxDiags = collector.CollectSymbols(fileUri, sourceCode, parser, table);

    SemanticAnalyzer analyzer;
    angel_lsp::i18n::I18n i18n("en");
    SemanticAnalysisRequest req{table, fileUri, "", &i18n};
    auto semanticDiags = analyzer.Analyze(req);

    if (expectedCode.empty())
    {
        return syntaxDiags.empty() && semanticDiags.empty();
    }

    for (const auto &d : syntaxDiags)
    {
        if (d.code == expectedCode) return true;
    }
    for (const auto &d : semanticDiags)
    {
        if (d.code == expectedCode) return true;
    }
    return false;
}

TEST_CASE("Group C1: Cast with Nested Template Types")
{
    SUBCASE("Variant 1: Cast to generic handle array<int>@")
    {
        std::string code = R"(
            class Dummy {}
            void main() {
                Dummy@ d = null;
                array<int>@ arr = cast<array<int>@>(d);
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 2: Cast to nested template handle array<array<float>>@")
    {
        std::string code = R"(
            class Dummy {}
            void main() {
                Dummy@ d = null;
                array<array<float>>@ arr = cast<array<array<float>>@>(d);
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 3: Cast in function argument expression")
    {
        std::string code = R"(
            class Dummy {}
            void process(array<int>@ a) {}
            void main() {
                Dummy@ d = null;
                process(cast<array<int>@>(d));
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 4: Invalid primitive cast target")
    {
        std::string code = R"(
            class Dummy {}
            void main() {
                Dummy@ d = null;
                int x = cast<int>(d);
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-unresolved-type"));
    }

    SUBCASE("Variant 5: Mixin class target in cast")
    {
        std::string code = R"(
            mixin class M {}
            class Dummy {}
            void main() {
                Dummy@ d = null;
                M@ m = cast<M@>(d);
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-mixin-not-a-type"));
    }

    SUBCASE("Variant 6: Unknown template argument in cast target")
    {
        std::string code = R"(
            class Dummy {}
            void main() {
                Dummy@ d = null;
                array<UnknownType>@ arr = cast<array<UnknownType>@>(d);
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-unresolved-type"));
    }

    SUBCASE("Variant 7: Cast to deep nested template with unknown inner type")
    {
        std::string code = R"(
            class Dummy {}
            void main() {
                Dummy@ d = null;
                array<array<TipoQueNoExiste>>@ arr = cast<array<array<TipoQueNoExiste>>@>(d);
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-unresolved-type"));
    }
}

TEST_CASE("Group C2: Compound Integer Expressions in Switch Case Labels")
{
    SUBCASE("Variant 1: Addition expression 1 + 1")
    {
        std::string code = R"(
            void main() {
                int x = 2;
                switch (x) {
                    case 1 + 1: break;
                }
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 2: Parenthesised and multiplied expression (2 * 3) - 1")
    {
        std::string code = R"(
            void main() {
                int x = 5;
                switch (x) {
                    case (2 * 3) - 1: break;
                }
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 3: Bitwise shift expression 1 << 4")
    {
        std::string code = R"(
            void main() {
                int x = 16;
                switch (x) {
                    case 1 << 4: break;
                }
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }
}

TEST_CASE("Group C3: Mixin Class Data Type Prohibition")
{
    SUBCASE("Variant 1: Mixin as local variable type")
    {
        std::string code = R"(
            mixin class MixinA { void foo() {} }
            void main() {
                MixinA obj;
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-mixin-not-a-type"));
    }

    SUBCASE("Variant 2: Mixin as function parameter type")
    {
        std::string code = R"(
            mixin class MixinA { void foo() {} }
            void test(MixinA param) {}
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-mixin-not-a-type"));
    }

    SUBCASE("Variant 3: Mixin as cast target type")
    {
        std::string code = R"(
            mixin class MixinA { void foo() {} }
            class Dummy {}
            void main() {
                Dummy d;
                cast<MixinA@>(d);
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-mixin-not-a-type"));
    }

    SUBCASE("Variant 4: Mixin used validly as class base")
    {
        std::string code = R"(
            mixin class MixinA { void foo() {} }
            class C : MixinA {}
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }
}

TEST_CASE("Group C4: Default Case Positional Rule")
{
    SUBCASE("Variant 1: Default case as last label")
    {
        std::string code = R"(
            void main() {
                int x = 1;
                switch (x) {
                    case 1: break;
                    default: break;
                }
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 2: Default case placed before case label")
    {
        std::string code = R"(
            void main() {
                int x = 1;
                switch (x) {
                    default: break;
                    case 1: break;
                }
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-default-must-be-last"));
    }

    SUBCASE("Variant 3: Multiple default cases")
    {
        std::string code = R"(
            void main() {
                int x = 1;
                switch (x) {
                    default: break;
                    default: break;
                }
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-default-must-be-last"));
    }
}

TEST_CASE("Group C5: Deep Namespace Scope Resolution")
{
    SUBCASE("Variant 1: 3-level namespace qualified type")
    {
        std::string code = R"(
            namespace N1 {
                namespace N2 {
                    namespace N3 {
                        class Target {}
                    }
                }
            }
            void main() {
                N1::N2::N3::Target obj;
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 2: 6-level namespace qualified type")
    {
        std::string code = R"(
            namespace A { namespace B { namespace C { namespace D { namespace E { namespace F {
                class Target {}
            }}}}}}
            void main() {
                A::B::C::D::E::F::Target obj;
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 3: 10-level namespace qualified type")
    {
        std::string code = R"(
            namespace N1 { namespace N2 { namespace N3 { namespace N4 { namespace N5 {
            namespace N6 { namespace N7 { namespace N8 { namespace N9 { namespace N10 {
                class Target {}
            }}}}}}}}}}
            void main() {
                N1::N2::N3::N4::N5::N6::N7::N8::N9::N10::Target obj;
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 4: 12-level namespace qualified type")
    {
        std::string code = R"(
            namespace N1{namespace N2{namespace N3{namespace N4{namespace N5{namespace N6{namespace N7{namespace N8{namespace N9{namespace N10{namespace N11{namespace N12{
                class Target{ void run(){} }
            }}}}}}}}}}}}
            void test(){ N1::N2::N3::N4::N5::N6::N7::N8::N9::N10::N11::N12::Target obj; obj.run(); }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 5: Qualified type in params, template arg, cast")
    {
        std::string code = R"(
            namespace N1{ namespace N2{ class Target{} } }
            void takesIt(N1::N2::Target@ t) {}
            array<N1::N2::Target@> arr;
            void test(){ N1::N2::Target@ x; cast<N1::N2::Target@>(x); }
        )";
        CHECK(TestSemanticCodeGroupC(code, ""));
    }

    SUBCASE("Variant 6: Unknown inner template argument in cast")
    {
        std::string code = R"(
            class Dummy {}
            void test() {
                Dummy@ a;
                cast<array<TipoQueNoExiste>@>(a);
            }
        )";
        CHECK(TestSemanticCodeGroupC(code, "as-err-unresolved-type"));
    }
}
