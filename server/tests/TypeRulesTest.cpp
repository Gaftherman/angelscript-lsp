#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "analysis/rules/TypeRules.h"
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
    std::vector<Diagnostic> AnalyzeTypeSnippet(const std::string &code,
                                               const std::string &fileUri = "file:///types.as")
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
// Typedef
// =====================================================================================

TEST_CASE("TypeRules - A typedef of a primitive is accepted")
{
    const auto diagnostics = AnalyzeTypeSnippet("typedef uint32 EntityId;\n");
    CHECK_FALSE(HasCode(diagnostics, "as-err-typedef-non-primitive"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-typedef-unresolved"));
}

TEST_CASE("TypeRules - A non-primitive typedef is left to the parser to report")
{
    // The grammar's typedef_declaration only accepts a primitive_type, so this never parses as a
    // typedef at all - it is recovered as a variable declaration and the parser reports the syntax
    // error. That is why as-err-typedef-non-primitive is deliberately not implemented here.
    const std::string code =
        "class Entity {}\n"
        "typedef Entity Alias;\n";

    const auto diagnostics = AnalyzeTypeSnippet(code);
    CHECK_FALSE(HasCode(diagnostics, "as-err-typedef-non-primitive"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-typedef-unresolved"));
}

// =====================================================================================
// Enum
// =====================================================================================

TEST_CASE("TypeRules - Integer initializers are accepted, in both bases")
{
    const std::string code =
        "enum Flags\n"
        "{\n"
        "    None = 0,\n"
        "    First = 1,\n"
        "    High = 0x80,\n"
        "    Negative = -1\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeTypeSnippet(code), "as-err-enum-invalid-initializer"));
}

TEST_CASE("TypeRules - Reports a string or floating point enum initializer")
{
    CHECK(HasCode(AnalyzeTypeSnippet("enum Bad { Value = 'text' }\n"),
                  "as-err-enum-invalid-initializer"));
    CHECK(HasCode(AnalyzeTypeSnippet("enum Bad { Value = 1.5 }\n"),
                  "as-err-enum-invalid-initializer"));
}

TEST_CASE("TypeRules - An initializer this pass cannot evaluate is left alone")
{
    // Referring to another member, a constant, or an expression is legal and this pass does not
    // evaluate expressions - so it must not guess.
    const std::string code =
        "enum Flags\n"
        "{\n"
        "    First = 1,\n"
        "    Second = First,\n"
        "    Both = First | Second\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeTypeSnippet(code), "as-err-enum-invalid-initializer"));
}

// =====================================================================================
// Funcdef
// =====================================================================================

TEST_CASE("TypeRules - Reports a handle on a primitive in a funcdef")
{
    CHECK(HasCode(AnalyzeTypeSnippet("funcdef int@ Broken();\n"), "as-err-handle-on-primitive"));
}

TEST_CASE("TypeRules - An ordinary funcdef is accepted")
{
    const std::string code =
        "class Entity {}\n"
        "funcdef bool Predicate(const Entity@ &in candidate);\n";

    CHECK_FALSE(HasCode(AnalyzeTypeSnippet(code), "as-err-handle-on-primitive"));
}

// =====================================================================================
// Interface members
// =====================================================================================

TEST_CASE("TypeRules - An interface constructor is left to the parser to report")
{
    // interface_method requires a return type, so `IThing();` is never collected as a member and
    // there is nothing in the symbol table for a rule to find. as-err-interface-constructor is
    // deliberately not implemented for that reason.
    CHECK_FALSE(HasCode(AnalyzeTypeSnippet("interface IThing { IThing(); void Do(); }\n"),
                        "as-err-interface-constructor"));
}

TEST_CASE("TypeRules - An ordinary interface is accepted")
{
    CHECK_FALSE(HasCode(AnalyzeTypeSnippet("interface IThing { void Do(); int Count() const; }\n"),
                        "as-err-interface-constructor"));
}

// =====================================================================================
// Duplicates and name conflicts
// =====================================================================================

TEST_CASE("TypeRules - Reports the same function declared twice with one signature")
{
    const std::string code =
        "void Spawn(int id) {}\n"
        "void Spawn(int id) {}\n";

    CHECK(HasCode(AnalyzeTypeSnippet(code), "as-err-duplicate-symbol"));
}

TEST_CASE("TypeRules - Overloads differing in parameters are not duplicates")
{
    const std::string code =
        "void Spawn(int id) {}\n"
        "void Spawn(const string &in name) {}\n"
        "void Spawn(int id, bool force) {}\n";

    CHECK_FALSE(HasCode(AnalyzeTypeSnippet(code), "as-err-duplicate-symbol"));
}

TEST_CASE("TypeRules - Reports a name used for two different kinds of declaration")
{
    const std::string code =
        "class Thing {}\n"
        "void Thing() {}\n";

    CHECK(HasCode(AnalyzeTypeSnippet(code), "as-err-name-conflict"));
}

TEST_CASE("TypeRules - The same name in two files of a module is not a redeclaration")
{
    // Every member of an #include module is indexed alongside the open document, so a shared
    // header's declarations arrive many times over. Judging those as duplicates would flag every
    // module that includes anything.
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    const std::string shared = "void Helper() {}\n";
    collector.CollectSymbols("file:///a.as", shared, parser, table);
    collector.CollectSymbols("file:///b.as", shared, parser, table);

    SemanticAnalysisRequest request{ table, "file:///a.as", ".as.predefined", &i18n };
    request.sourceCode = shared;

    SemanticAnalyzer analyzer(nullptr);
    CHECK_FALSE(HasCode(analyzer.Analyze(request), "as-err-duplicate-symbol"));
}

TEST_CASE("TypeRules - A predefined stub is exempt")
{
    const auto diagnostics = AnalyzeTypeSnippet("typedef CBaseEntity Alias;\nenum E { V = 'x' }\n",
                                                "file:///engine.as.predefined");
    CHECK_FALSE(HasCode(diagnostics, "as-err-typedef-non-primitive"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-enum-invalid-initializer"));
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Type Rules Corpus Audit*"`)
// =====================================================================================

TEST_CASE("TypeRules - Type Rules Corpus Audit" * doctest::skip(true))
{
    static const std::vector<std::string> k_codes = {
        "as-err-typedef-non-primitive", "as-err-typedef-unresolved", "as-err-enum-invalid-initializer",
        "as-err-interface-constructor", "as-err-duplicate-symbol", "as-err-name-conflict",
        "as-err-handle-on-primitive", "as-syntax-error-missing",
        "as-err-declaration-missing-body", "as-err-external-not-shared"
    };

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return std::find(k_codes.begin(), k_codes.end(), code) != k_codes.end();
    });

    MESSAGE("Type-rule corpus audit: files=" << result.filesAnalysed
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

    // Exactly one finding survives triage, and it is a genuine one: angel-lsp_diagnostics.as
    // declares `void repeated() { }` twice, on purpose, as a fixture for this very diagnostic.
    // Everything else the rule used to report over the corpus was a false positive and was fixed
    // rather than suppressed - see the notes in TypeRules.cpp for each.
    CHECK(result.Total() == 1);
    REQUIRE(result.hits.size() == 1);
    CHECK(result.hits[0].fileName == "angel-lsp_diagnostics.as");
    CHECK(result.hits[0].code == "as-err-duplicate-symbol");
}
