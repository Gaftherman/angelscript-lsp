#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    std::vector<Diagnostic> AnalyzeFunctionSnippet(const std::string &code,
                                                   const std::string &fileUri = "file:///funcs.as")
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static angel_lsp::i18n::I18n i18n;

        collector.CollectSymbols(fileUri, code, parser, table);

        SemanticAnalysisRequest request{ table, fileUri, ".as.predefined", &i18n };
        request.scopeRoot = scopes.CollectScopes(code, parser);
        request.sourceCode = code;

        SemanticAnalyzer analyzer(nullptr);
        return analyzer.Analyze(request);
    }

    bool HasCode(const std::vector<Diagnostic> &diagnostics, const std::string &code)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&code](const Diagnostic &diag) { return diag.code == code; });
    }
}

// =====================================================================================
// Body
// =====================================================================================

TEST_CASE("FunctionRules - Reports a function declared without a body")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void Orphan();\n"), "as-err-missing-body"));
}

TEST_CASE("FunctionRules - An interface method needs no body")
{
    const std::string code =
        "interface IThinker\n"
        "{\n"
        "    void Think();\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-missing-body"));
}

TEST_CASE("FunctionRules - An external method needs no body")
{
    const std::string code =
        "shared class Entity\n"
        "{\n"
        "    external void Think();\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-missing-body"));
}

TEST_CASE("FunctionRules - Reports a deleted function given a body")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    Entity(const Entity &in other) delete { }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-delete-with-body"));
}

TEST_CASE("FunctionRules - Reports a deleted function carrying another qualifier")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    void Think() const delete;\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-delete-with-other-qualifier"));
}

// =====================================================================================
// Function attributes: delete, explicit, property
//
// Every expectation below was compiled with a real AngelScript build before it was written down,
// so where a case looks surprising - a two-parameter `explicit` constructor, a by-value copy
// constructor, a getter that takes an index - the engine is the reason.
// =====================================================================================

TEST_CASE("FunctionRules - The three auto generated functions may be deleted")
{
    // Only these exist for a script class, which is the whole meaning of `delete`: do not generate
    // this one. The reference modifier and const take no part - the engine accepts `A(A a)`,
    // `A(A &inout)` and `A(const A &in)` alike as the copy constructor.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    Entity() delete;\n"
        "    Entity(const Entity &in other) delete;\n"
        "    Entity &opAssign(const Entity &in other) delete;\n"
        "    Entity(int id) { }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-delete-not-auto-generated"));
}

TEST_CASE("FunctionRules - Reports a deleted method the engine never generates")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    void Think() delete;\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-delete-not-auto-generated"));
}

TEST_CASE("FunctionRules - Reports a deleted constructor that is not the default or copy one")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    Entity(int id) delete;\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-delete-not-auto-generated"));
}

TEST_CASE("FunctionRules - Reports a deleted opAssign with a signature that is not generated")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    Entity &opAssign(int value) delete;\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-delete-not-auto-generated"));
}

TEST_CASE("FunctionRules - Reports a deleted global function")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void Think() delete;\n"), "as-err-delete-not-auto-generated"));
}

TEST_CASE("FunctionRules - A deleted destructor keeps its own diagnostic")
{
    // as-err-destructor-delete says exactly what is wrong; repeating it as "not auto generated"
    // would be true but less useful, so the general rule stands aside for it.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    ~Entity() delete;\n"
        "}\n";

    const auto diagnostics = AnalyzeFunctionSnippet(code);
    CHECK(HasCode(diagnostics, "as-err-destructor-delete"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-delete-not-auto-generated"));
}

TEST_CASE("FunctionRules - Reports explicit on a global function")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void Convert() explicit { }\n"), "as-err-explicit-not-member"));
}

TEST_CASE("FunctionRules - Reports every function attribute on an interface method")
{
    // The engine rejects all five, each with its own parse error. The grammar parses them so this
    // can name the offender instead of leaving the user a syntax error on the token.
    for (const std::string attribute : { "override", "final", "explicit", "property", "delete" })
    {
        const std::string code =
            "interface IThinker\n"
            "{\n"
            "    void Think() " + attribute + ";\n"
            "}\n";

        INFO("attribute: ", attribute);
        const auto diagnostics = AnalyzeFunctionSnippet(code);
        CHECK(HasCode(diagnostics, "as-err-interface-method-attribute"));
        CHECK_FALSE(HasCode(diagnostics, "as-syntax-error"));
    }
}

TEST_CASE("FunctionRules - An ordinary interface method carries no attribute finding")
{
    // `const` is not a function attribute - the grammar spells it separately and an interface
    // method may carry it.
    const std::string code =
        "interface IThinker\n"
        "{\n"
        "    void Think();\n"
        "    int Count() const;\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-interface-method-attribute"));
}

TEST_CASE("FunctionRules - explicit is accepted on any class method, whatever its arity")
{
    // Not only on a single-argument constructor: the engine takes it on a two-argument one and on
    // an ordinary method without complaint, so neither is reported here.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    Entity(int id) explicit { }\n"
        "    Entity(int id, int team) explicit { }\n"
        "    void Think() explicit { }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-explicit-not-member"));
}

TEST_CASE("FunctionRules - Accepts the virtual property accessor signatures the engine accepts")
{
    // The index parameter is the reason a getter takes one argument and a setter two; the corpus
    // writes both, and `a[i].prop` is what they are for.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    int health;\n"
        "    int get_hp() const property { return health; }\n"
        "    void set_hp(int value) property { health = value; }\n"
        "    int get_slot(int index) property { return index; }\n"
        "    void set_slot(int index, int value) property { }\n"
        "}\n"
        "int get_globalCount() property { return 0; }\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-virtual-property-signature"));
}

TEST_CASE("FunctionRules - Reports a property attribute on a name that is not an accessor")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    void Think() property { }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-virtual-property-signature"));
}

TEST_CASE("FunctionRules - Reports a getter that returns nothing and a setter that takes nothing")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    void get_hp() property { }\n"
        "    void set_hp() property { }\n"
        "}\n";

    const auto diagnostics = AnalyzeFunctionSnippet(code);
    CHECK(std::count_if(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic &diag)
                        { return diag.code == "as-err-virtual-property-signature"; }) == 2);
}

TEST_CASE("FunctionRules - Reports an accessor carrying more than an index")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    int get_slot(int index, int extra) property { return 0; }\n"
        "    void set_slot(int index, int extra, int value) property { }\n"
        "}\n";

    const auto diagnostics = AnalyzeFunctionSnippet(code);
    CHECK(std::count_if(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic &diag)
                        { return diag.code == "as-err-virtual-property-signature"; }) == 2);
}

TEST_CASE("FunctionRules - A stub declares attributes without being judged for them")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    void Think() delete;\n"
        "    void Reset() explicit;\n"
        "    void Broken() property;\n"
        "}\n";

    const auto diagnostics = AnalyzeFunctionSnippet(code, "file:///as.predefined");
    CHECK_FALSE(HasCode(diagnostics, "as-err-delete-not-auto-generated"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-explicit-not-member"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-virtual-property-signature"));
}

// =====================================================================================
// Return type
// =====================================================================================

TEST_CASE("FunctionRules - Reports a const void return type")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("const void Nothing() { }\n"), "as-err-const-void-return"));
}

TEST_CASE("FunctionRules - Reports a reference to void as a return type")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void& Nothing() { }\n"), "as-err-void-reference"));
}

TEST_CASE("FunctionRules - A reference return is not judged")
{
    // `int &Function()` is the engine's own documented example, legal whenever the host built with
    // asEP_ALLOW_UNSAFE_REFERENCES - a build option no reader of script text can observe.
    const std::string code =
        "int g_property;\n"
        "int& Function() { return g_property; }\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-invalid-reference-return"));
}

TEST_CASE("FunctionRules - Reports a handle on a primitive return type")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("int@ Broken() { return null; }\n"),
                  "as-err-handle-on-primitive"));
}

TEST_CASE("FunctionRules - Reports a mixin used as a return type")
{
    const std::string code =
        "mixin class Helper {}\n"
        "Helper Make() { }\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-mixin-not-a-type"));
}

TEST_CASE("FunctionRules - An ordinary return type is not judged")
{
    const std::string code =
        "class Entity {}\n"
        "Entity@ Spawn() { return null; }\n"
        "array<int>@ Numbers() { return null; }\n"
        "const string& Name() { }\n";

    const auto diagnostics = AnalyzeFunctionSnippet(code);
    CHECK_FALSE(HasCode(diagnostics, "as-err-handle-on-primitive"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-void-reference"));
}

TEST_CASE("FunctionRules - A declaration with a constructor initializer is not a missing body")
{
    // The grammar reads `Type name(args);` as a body-less function, which is what every global or
    // field declared with a constructor initializer looks like from here. AngelScript has no
    // prototypes, so the variable reading is the only correct one.
    const std::string code =
        "class Beam {}\n"
        "const uint MAX = 30;\n"
        "array<string> g_names(MAX);\n"
        "array<Beam@> g_beams();\n"
        "class Slave\n"
        "{\n"
        "    private array<Beam@> m_beams(MAX);\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-missing-body"));
}

TEST_CASE("FunctionRules - A prototype no variable declaration could be is still reported")
{
    // A void return type or a named parameter: neither can occur in an argument list, so these
    // cannot be read as variable declarations.
    CHECK(HasCode(AnalyzeFunctionSnippet("void Think();\n"), "as-err-missing-body"));
    CHECK(HasCode(AnalyzeFunctionSnippet("void Think(int radius);\n"), "as-err-missing-body"));
    CHECK(HasCode(AnalyzeFunctionSnippet("int Join(int radius);\n"), "as-err-missing-body"));
}

// =====================================================================================
// Modifiers and placement
// =====================================================================================

TEST_CASE("FunctionRules - Reports const on a global function as an error")
{
    // The engine's parser refuses the token: "Instead found reserved keyword 'const'".
    CHECK(HasCode(AnalyzeFunctionSnippet("void Think() const { }\n"),
                  "as-err-global-function-qualifiers"));
}

TEST_CASE("FunctionRules - Reports override and final on a global function as a warning")
{
    // Not an error: the engine accepts both on a global function and silently ignores them, so
    // reporting them as errors would be this analyzer inventing a rule AngelScript does not have.
    // Still worth saying, because a global marked `override` is usually a method that lost its
    // class - which is what a warning is for.
    for (const std::string source : { "void Think() override { }\n", "void Think() final { }\n" })
    {
        const auto diagnostics = AnalyzeFunctionSnippet(source);
        CHECK_FALSE(HasCode(diagnostics, "as-err-global-function-qualifiers"));

        const auto found = std::find_if(diagnostics.begin(), diagnostics.end(),
                                        [](const Diagnostic &diag)
                                        { return diag.code == "as-warn-global-function-attribute"; });
        REQUIRE(found != diagnostics.end());
        CHECK(found->severity == DiagnosticSeverity::Warning);
    }
}

TEST_CASE("FunctionRules - Member qualifiers on a method are accepted")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    int Health() const { return 0; }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-global-function-qualifiers"));
}

// =====================================================================================
// Constructors and destructors
// =====================================================================================

TEST_CASE("FunctionRules - Reports a destructor declared with parameters")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    ~Entity(int mode) { }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-destructor-param"));
}

TEST_CASE("FunctionRules - Reports a destructor declared with a return type")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    void ~Entity() { }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-destructor-return-type"));
}

TEST_CASE("FunctionRules - An ordinary constructor and destructor pass")
{
    const std::string code =
        "class Entity\n"
        "{\n"
        "    Entity() { }\n"
        "    ~Entity() { }\n"
        "}\n";

    const auto diagnostics = AnalyzeFunctionSnippet(code);
    CHECK_FALSE(HasCode(diagnostics, "as-err-destructor-param"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-destructor-return-type"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-missing-body"));
}

TEST_CASE("FunctionRules - Reports a constructor on a mixin class")
{
    const std::string code =
        "mixin class Helper\n"
        "{\n"
        "    Helper() { }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-mixin-constructor"));
}

TEST_CASE("FunctionRules - Reports a destructor on a mixin class")
{
    const std::string code =
        "mixin class Helper\n"
        "{\n"
        "    ~Helper() { }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-mixin-destructor"));
}

// =====================================================================================
// Override
// =====================================================================================

TEST_CASE("FunctionRules - Reports override on a method no base declares")
{
    const std::string code =
        "class Base\n"
        "{\n"
        "    void Think() { }\n"
        "}\n"
        "class Derived : Base\n"
        "{\n"
        "    void Act() override { }\n"
        "}\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-override-no-base"));
}

TEST_CASE("FunctionRules - Override of a method a base declares is accepted")
{
    const std::string code =
        "class Base\n"
        "{\n"
        "    void Think() { }\n"
        "}\n"
        "class Derived : Base\n"
        "{\n"
        "    void Think() override { }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-override-no-base"));
}

TEST_CASE("FunctionRules - Override of a method a grandparent declares is accepted")
{
    const std::string code =
        "class Base\n"
        "{\n"
        "    void Think() { }\n"
        "}\n"
        "class Middle : Base\n"
        "{\n"
        "}\n"
        "class Derived : Middle\n"
        "{\n"
        "    void Think() override { }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-override-no-base"));
}

TEST_CASE("FunctionRules - Override is not judged when a base is invisible")
{
    // CBaseEntity is registered by the host, so nothing here can say what it declares. The rule
    // must stay silent rather than assume the method is not there.
    const std::string code =
        "class Derived : CBaseEntity\n"
        "{\n"
        "    void Spawn() override { }\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-override-no-base"));
}

// =====================================================================================
// Parameters
// =====================================================================================

TEST_CASE("FunctionRules - Reports a duplicated parameter name")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void Move(int x, int x) { }\n"), "as-err-duplicate-param"));
}

TEST_CASE("FunctionRules - Reports a parameter declared void")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void Move(void x, int y) { }\n"), "as-err-void-parameter"));
}

TEST_CASE("FunctionRules - A lone unnamed void means no parameters")
{
    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet("void Think(void) { }\n"), "as-err-void-parameter"));
}

TEST_CASE("FunctionRules - Reports a defaulted parameter followed by a plain one")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void Move(int x = 0, int y) { }\n"),
                  "as-err-default-param-order"));
}

TEST_CASE("FunctionRules - Defaults at the end of the list are accepted")
{
    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet("void Move(int x, int y = 0, int z = 0) { }\n"),
                        "as-err-default-param-order"));
}

TEST_CASE("FunctionRules - Reports inout on a primitive parameter")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void Move(int &inout x) { }\n"), "as-err-inout-on-primitive"));
}

TEST_CASE("FunctionRules - inout on an object parameter is accepted")
{
    const std::string code =
        "class Entity {}\n"
        "void Move(Entity &inout e) { }\n"
        "void Rename(string &inout name) { }\n";

    CHECK_FALSE(HasCode(AnalyzeFunctionSnippet(code), "as-err-inout-on-primitive"));
}

TEST_CASE("FunctionRules - Reports a handle on a primitive parameter")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("void Take(int@ value) { }\n"), "as-err-handle-on-primitive"));
}

TEST_CASE("FunctionRules - Reports a funcdef parameter declared without a handle")
{
    const std::string code =
        "funcdef void Callback();\n"
        "void Register(Callback cb) { }\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-funcdef-not-handle"));
}

TEST_CASE("FunctionRules - Reports a mixin used as a parameter type")
{
    const std::string code =
        "mixin class Helper {}\n"
        "void Use(Helper h) { }\n";

    CHECK(HasCode(AnalyzeFunctionSnippet(code), "as-err-mixin-not-a-type"));
}

TEST_CASE("FunctionRules - An ordinary parameter list is not judged")
{
    const std::string code =
        "class Entity {}\n"
        "void Spawn(Entity@ e, const string &in name, array<int>@ ids, float delay = 0.0f) { }\n";

    const auto diagnostics = AnalyzeFunctionSnippet(code);
    CHECK_FALSE(HasCode(diagnostics, "as-err-handle-on-primitive"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-inout-on-primitive"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-void-parameter"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-default-param-order"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-duplicate-param"));
}

TEST_CASE("FunctionRules - A funcdef's parameter list is validated too")
{
    CHECK(HasCode(AnalyzeFunctionSnippet("funcdef void Callback(int x, int x);\n"),
                  "as-err-duplicate-param"));
}

TEST_CASE("FunctionRules - A predefined stub is exempt")
{
    const auto diagnostics = AnalyzeFunctionSnippet("void Orphan();\nvoid Think() const;\n",
                                                    "file:///engine.as.predefined");
    CHECK_FALSE(HasCode(diagnostics, "as-err-missing-body"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-global-function-qualifiers"));
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Function Rules Corpus Audit*"`)
// =====================================================================================

TEST_CASE("FunctionRules - Function Rules Corpus Audit" * doctest::skip(true))
{
    static const std::vector<std::string> k_codes = {
        "as-err-missing-body", "as-err-delete-with-body", "as-err-delete-with-other-qualifier",
        "as-err-const-void-return", "as-err-void-reference",
        "as-err-global-function-qualifiers", "as-err-destructor-param",
        "as-err-destructor-return-type", "as-err-destructor-delete", "as-err-mixin-constructor",
        "as-err-mixin-destructor", "as-err-override-no-base", "as-err-duplicate-param",
        "as-err-void-parameter", "as-err-default-param-order", "as-err-inout-on-primitive",
        "as-err-double-reference", "as-err-delete-not-auto-generated",
        "as-err-explicit-not-member", "as-err-virtual-property-signature",
        "as-err-interface-method-attribute", "as-warn-global-function-attribute"
    };

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return std::find(k_codes.begin(), k_codes.end(), code) != k_codes.end();
    });

    MESSAGE("Function-rule corpus audit: files=" << result.filesAnalysed
            << " totalFlagged=" << result.Total()
            << " seconds=" << result.seconds);

    for (const auto &[code, count] : result.countByCode)
    {
        MESSAGE("  " << code << ": " << count);
    }
    for (const auto &hit : result.hits)
    {
        MESSAGE("  " << hit.fileName << ":" << hit.line << " [" << hit.code << "] " << hit.message);
    }

    CHECK(result.filesAnalysed > 0);

    // What survives triage is two files that are not AngelScript at all but C++ headers saved as
    // .as - AngelScript-SvenCoop_oldone.as and AngelScripts_CSquadMonster.as, both full of
    // `public:` labels, `virtual`, `Schedule_t *` and a trailing `};`. Every method they declare is
    // a prototype, which AngelScript has no notion of, so reporting them is right. The parser
    // reports them too. Everything else the rules found over the corpus was a false positive and
    // was fixed at its source.
    CHECK(result.Total() == 22);
    CHECK(result.countByCode.size() == 1);
    CHECK(result.countByCode.count("as-err-missing-body") == 1);

    for (const auto &hit : result.hits)
    {
        CHECK((hit.fileName == "AngelScript-SvenCoop_oldone.as" ||
               hit.fileName == "AngelScripts_CSquadMonster.as"));
    }
}
