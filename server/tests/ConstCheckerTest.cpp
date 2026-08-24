#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
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

// =====================================================================================
// Const correctness at the use site.
//
// Every expectation here was compiled with a real AngelScript build first, and one of the answers
// is the opposite of what C++ would give: calling a non-const method from inside a const one,
// through the implicit `this`, is accepted. There is a test below pinning that, because the
// obvious rule is the wrong rule.
// =====================================================================================

namespace
{
    std::vector<Diagnostic> AnalyzeConstSnippet(const std::string &code,
                                                const std::string &fileUri = "file:///const.as")
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
        request.tree = parser.Parse(code);

        SemanticAnalyzer analyzer(nullptr);
        auto diagnostics = analyzer.Analyze(request);

        if (request.tree)
        {
            ts_tree_delete(const_cast<TSTree *>(request.tree));
        }
        return diagnostics;
    }

    bool HasCode(const std::vector<Diagnostic> &diagnostics, const std::string &code)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&code](const Diagnostic &diag) { return diag.code == code; });
    }

    bool HasNoConstFinding(const std::vector<Diagnostic> &diagnostics)
    {
        return !HasCode(diagnostics, "as-err-const-assignment") &&
               !HasCode(diagnostics, "as-err-const-method-required");
    }
}

// =====================================================================================
// Assignment
// =====================================================================================

TEST_CASE("ConstChecker - Reports an assignment to a const global")
{
    const std::string code =
        "const int g_max = 10;\n"
        "void main() { g_max = 5; }\n";

    CHECK(HasCode(AnalyzeConstSnippet(code), "as-err-const-assignment"));
}

TEST_CASE("ConstChecker - Reports an assignment to a const local")
{
    const std::string code =
        "void main()\n"
        "{\n"
        "    const int limit = 10;\n"
        "    limit = 5;\n"
        "}\n";

    CHECK(HasCode(AnalyzeConstSnippet(code), "as-err-const-assignment"));
}

TEST_CASE("ConstChecker - Reports a field written through a const reference")
{
    // The engine answers this with the same sentence it gives a const global - "Expression is not
    // an l-value" - because a member of a const object is const.
    const std::string code =
        "class Entity { int v; }\n"
        "void Take(const Entity &in e) { e.v = 5; }\n";

    CHECK(HasCode(AnalyzeConstSnippet(code), "as-err-const-assignment"));
}

TEST_CASE("ConstChecker - A mutable target is left alone")
{
    const std::string code =
        "int g_count = 0;\n"
        "class Entity { int v; }\n"
        "void Take(Entity &inout e)\n"
        "{\n"
        "    int local = 0;\n"
        "    local = 1;\n"
        "    g_count = 2;\n"
        "    e.v = 3;\n"
        "}\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code)));
}

TEST_CASE("ConstChecker - A compound assignment counts as an assignment")
{
    const std::string code =
        "const int g_max = 10;\n"
        "void main() { g_max += 5; }\n";

    CHECK(HasCode(AnalyzeConstSnippet(code), "as-err-const-assignment"));
}

// =====================================================================================
// Method calls
// =====================================================================================

TEST_CASE("ConstChecker - Reports a non-const method called through a const object")
{
    const std::string code =
        "class Entity { int v; void Mutate() { v = 1; } }\n"
        "void Take(const Entity &in e) { e.Mutate(); }\n";

    CHECK(HasCode(AnalyzeConstSnippet(code), "as-err-const-method-required"));
}

TEST_CASE("ConstChecker - Reports it through a const handle too")
{
    // `const Entity@` is a handle to a read-only object, which is the case the engine refuses.
    const std::string code =
        "class Entity { int v; void Mutate() { v = 1; } }\n"
        "void Take(const Entity@ e) { e.Mutate(); }\n";

    CHECK(HasCode(AnalyzeConstSnippet(code), "as-err-const-method-required"));
}

TEST_CASE("ConstChecker - A const method called through a const object is correct")
{
    const std::string code =
        "class Entity { int v; int Read() const { return v; } }\n"
        "void Take(const Entity &in e) { e.Read(); }\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code)));
}

TEST_CASE("ConstChecker - One const overload in the set makes the call legal")
{
    // The engine's message is a failed overload lookup - "No matching signatures to
    // 'Entity::Get() const'" - not a refusal of the call, so it finds the const one and compiles.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    int v;\n"
        "    int Get() { return v; }\n"
        "    int Get() const { return v; }\n"
        "}\n"
        "void Take(const Entity &in e) { e.Get(); }\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code)));
}

TEST_CASE("ConstChecker - A const method inherited from a base is found")
{
    const std::string code =
        "class Base { int v; int Read() const { return v; } }\n"
        "class Derived : Base { }\n"
        "void Take(const Derived &in d) { d.Read(); }\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code)));
}

TEST_CASE("ConstChecker - A non-const method through a mutable object is left alone")
{
    const std::string code =
        "class Entity { int v; void Mutate() { v = 1; } }\n"
        "void Take(Entity &inout e) { e.Mutate(); }\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code)));
}

// =====================================================================================
// The rule that is not there
// =====================================================================================

TEST_CASE("ConstChecker - A const method may call a non-const one on itself")
{
    // C++ rejects this and AngelScript compiles it, checked against a real engine rather than
    // assumed. `this` is therefore never treated as const, and there is no rule here to write.
    const std::string code =
        "class Entity\n"
        "{\n"
        "    int v;\n"
        "    void Mutate() { v = 1; }\n"
        "    void Read() const { Mutate(); }\n"
        "}\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code)));
}

// =====================================================================================
// Silence where nothing can be established
// =====================================================================================

TEST_CASE("ConstChecker - A type this analyzer cannot see is never judged")
{
    // An engine-registered type carries const overloads written down in no source here, so a
    // method call through one is not something this pass may have an opinion about.
    const std::string code =
        "void Take(const CBaseEntity@ ent)\n"
        "{\n"
        "    ent.TakeDamage();\n"
        "}\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code)));
}

TEST_CASE("ConstChecker - A base that does not resolve keeps the whole type unjudged")
{
    const std::string code =
        "class Derived : CBaseEntity { void Mutate() { } }\n"
        "void Take(const Derived &in d) { d.Mutate(); }\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code)));
}

TEST_CASE("ConstChecker - A predefined stub is exempt")
{
    const std::string code =
        "const int g_max = 10;\n"
        "void main() { g_max = 5; }\n";

    CHECK(HasNoConstFinding(AnalyzeConstSnippet(code, "file:///engine.as.predefined")));
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Const Corpus Audit*"`)
// =====================================================================================

TEST_CASE("ConstChecker - Const Corpus Audit" * doctest::skip(true))
{
    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return code == "as-err-const-assignment" || code == "as-err-const-method-required";
    });

    MESSAGE("Const corpus audit: files=" << result.filesAnalysed
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
}
