#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "helpers/TestUtils.h"
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

        auto diagnostics = collector.CollectSymbols(fileUri, code, parser, table, &i18n);

        SemanticAnalysisRequest request{ table, fileUri, ".as.predefined", &i18n };
        request.scopeRoot = scopes.CollectScopes(code, parser);
        request.sourceCode = code;
        request.tree = parser.Parse(code);

        SemanticAnalyzer analyzer(nullptr);
        auto semDiags = analyzer.Analyze(request);
        diagnostics.insert(diagnostics.end(), semDiags.begin(), semDiags.end());

        if (request.tree)
        {
            ts_tree_delete(const_cast<TSTree *>(request.tree));
        }
        return diagnostics;
    }

    std::vector<Diagnostic> FilterErrors(const std::vector<Diagnostic> &diagnostics)
    {
        std::vector<Diagnostic> errors;
        for (const auto &d : diagnostics)
        {
            if (d.severity == DiagnosticSeverity::Error)
            {
                errors.push_back(d);
            }
        }
        std::sort(errors.begin(), errors.end(), [](const Diagnostic &a, const Diagnostic &b)
        {
            if (a.range.start.line != b.range.start.line)
            {
                return a.range.start.line < b.range.start.line;
            }
            return a.range.start.character < b.range.start.character;
        });
        return errors;
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
}

TEST_CASE("TypeRules - Reports a typedef of a user-defined type")
{
    // AngelScript typedefs a primitive and nothing else; its own parser refuses the rest outright.
    // The grammar parses it now so the user gets this sentence instead of a syntax error pointing
    // at the alias name.
    const std::string code =
        "class Entity {}\n"
        "typedef Entity Alias;\n";

    const auto diagnostics = AnalyzeTypeSnippet(code);
    CHECK(HasCode(diagnostics, "as-err-typedef-non-primitive"));
    CHECK_FALSE(HasCode(diagnostics, "as-syntax-error"));
}

TEST_CASE("TypeRules - A typedef of a name that resolves to nothing reads the same way")
{
    // There is no second failure mode to distinguish. Whether the name is a class, an enum or a
    // typo, it is not a primitive, and the engine never gets far enough to look it up - which is
    // why as-err-typedef-unresolved described a condition AngelScript does not have and is gone.
    CHECK(HasCode(AnalyzeTypeSnippet("typedef Missing Alias;\n"), "as-err-typedef-non-primitive"));
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
    CHECK_FALSE(HasCode(AnalyzeTypeSnippet(code), "as-err-funcdef-attribute"));
}

TEST_CASE("TypeRules - Reports every function attribute on a funcdef")
{
    // A funcdef names a signature: no body, no class, nothing to override. The engine rejects all
    // five attributes on one, and the grammar parses them so this can name which.
    for (const std::string attribute : { "override", "final", "explicit", "property", "delete" })
    {
        INFO("attribute: ", attribute);
        const auto diagnostics = AnalyzeTypeSnippet("funcdef void Callback() " + attribute + ";\n");
        CHECK(HasCode(diagnostics, "as-err-funcdef-attribute"));
        CHECK_FALSE(HasCode(diagnostics, "as-syntax-error"));
    }
}

TEST_CASE("TypeRules - A shared funcdef is not mistaken for one carrying an attribute")
{
    // 'shared' and 'external' are declaration modifiers, not function attributes, and they are the
    // two a funcdef legitimately carries.
    CHECK_FALSE(HasCode(AnalyzeTypeSnippet("shared funcdef void Callback();\n"),
                        "as-err-funcdef-attribute"));
}

// =====================================================================================
// Interface members
// =====================================================================================

TEST_CASE("TypeRules - Reports a constructor or a destructor declared in an interface")
{
    // An interface declares a contract, never construction. The engine refuses both forms and the
    // grammar parses them now, so this names the construct rather than pointing at a token.
    CHECK(HasCode(AnalyzeTypeSnippet("interface IThing { IThing(); void Do(); }\n"),
                  "as-err-interface-constructor"));
    CHECK(HasCode(AnalyzeTypeSnippet("interface IThing { ~IThing(); void Do(); }\n"),
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
    const auto diagnostics = AnalyzeTypeSnippet("enum E { V = 'x' }\n",
                                                "file:///engine.as.predefined");
    CHECK_FALSE(HasCode(diagnostics, "as-err-enum-invalid-initializer"));
}

TEST_SUITE("AngelScript_CleanPredefined_And_DuplicateDetection")
{
    TEST_CASE("Clean Slate: No ghost types exist without physical workspace files")
    {
        const std::string script =
            "void Main() {\n"
            "    string s; // Error: 'string' does not exist in an empty workspace\n"
            "}\n";

        auto rawDiagnostics = AnalyzeTypeSnippet(script, "file:///workspace/main.as");
        auto errors = FilterErrors(rawDiagnostics);
        REQUIRE_FALSE(errors.empty());
        CHECK(errors[0].code == "as-err-unresolved-type");
        CHECK(errors[0].range.start.line == 1);
    }

    TEST_CASE("Predefined Diagnostics: Detect duplicate method definitions in class")
    {
        const std::string predefinedScript =
            "class string {\n"
            "    bool isEmpty() const;\n"
            "    uint Length() const;\n"
            "    bool isEmpty() const; // Error: Duplicate method declaration\n"
            "}\n";

        auto rawDiagnostics = AnalyzeTypeSnippet(predefinedScript, "file:///workspace/as.predefined");
        auto errors = FilterErrors(rawDiagnostics);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == "as-err-duplicate-symbol");
        CHECK(errors[0].range.start.line == 3);
    }

    TEST_CASE("Predefined Diagnostics: Allow valid method overloading while catching duplicates")
    {
        const std::string predefinedScript =
            "class Vector3 {\n"
            "    float Length() const;\n"
            "    Vector3 opAdd(const Vector3 &in other) const; // Overload 1 (OK)\n"
            "    Vector3 opAdd(float scalar) const;            // Overload 2 (OK)\n"
            "    Vector3 opAdd(float scalar) const;            // Error: Duplicate Overload 2\n"
            "}\n";

        auto rawDiagnostics = AnalyzeTypeSnippet(predefinedScript, "file:///workspace/as.predefined");
        auto errors = FilterErrors(rawDiagnostics);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == "as-err-duplicate-symbol");
        CHECK(errors[0].range.start.line == 4);
    }

    TEST_CASE("Global Scope: Detect duplicate global function declarations")
    {
        const std::string predefinedScript =
            "void Print(const string &in text);\n"
            "float GetTime();\n"
            "void Print(const string &in text); // Error: Duplicate global function\n";

        auto rawDiagnostics = AnalyzeTypeSnippet(predefinedScript, "file:///workspace/as.predefined");
        auto errors = FilterErrors(rawDiagnostics);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == "as-err-duplicate-symbol");
        CHECK(errors[0].range.start.line == 2);
    }
}

TEST_SUITE("AngelScript_DuplicateDeclaration_Diagnostics")
{
    TEST_CASE("Global Scope: Detect exact duplicate global function declarations in as.predefined")
    {
        const char *predefinedScript = R"(
            /**
             * @brief Imprime un mensaje de texto en la consola de salida
             * @param text Contenido a imprimir
             */
            void Print(const string &in text);
            void Print(const string &in text); // Must emit E_DUPLICATE_DECLARATION
        )";

        auto doc = CreateTestDocument("file:///workspace/as.predefined", predefinedScript);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);

        CHECK(diagnostics[0].code == "E_DUPLICATE_DECLARATION");
        CHECK(diagnostics[0].range.start.line == 6);
        CHECK(diagnostics[0].range.start.character == 17); // Exact span over 'Print'
    }

    TEST_CASE("Class Scope: Detect duplicate method in as.predefined")
    {
        const char *predefinedScript = R"(
            class string {
                bool isEmpty() const;
                uint Length() const;
                bool isEmpty() const; // Must emit E_DUPLICATE_DECLARATION
            }
        )";

        auto doc = CreateTestDocument("file:///workspace/as.predefined", predefinedScript);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);

        CHECK(diagnostics[0].code == "E_DUPLICATE_DECLARATION");
        CHECK(diagnostics[0].range.start.line == 4);
    }

    TEST_CASE("Valid Overloads: Do not flag functions with distinct parameter types")
    {
        const char *predefinedScript = R"(
            void Print(const string &in text);
            void Print(int number);
            void Print(float value);
        )";

        auto doc = CreateTestDocument("file:///workspace/as.predefined", predefinedScript);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        CHECK(diagnostics.empty());
    }
}

TEST_SUITE("AngelScript_TypedefValidation_Parity")
{
    TEST_CASE("Valid Typedefs: Primitive types in as.predefined and script files")
    {
        const char *predefinedScript = R"(
            typedef float real32;
            typedef double real64;
            typedef uint32 EntityID;
        )";

        auto doc = CreateTestDocument("file:///workspace/as.predefined", predefinedScript);
        REQUIRE(doc != nullptr);
        CHECK(doc->GetDiagnostics().empty());

        // Validate that primitive aliases resolve correctly in symbol table
        CHECK(doc->GetSymbolTypeAt({1, 26}) == "float");
    }

    TEST_CASE("Invalid Typedefs: Reject class types in as.predefined")
    {
        const char *predefinedScript = R"(
            class string {}
            class Vector3 {}

            typedef string super_string; // Error: string is not a primitive
            typedef Vector3 Vec3;        // Error: Vector3 is not a primitive
        )";

        auto doc = CreateTestDocument("file:///workspace/as.predefined", predefinedScript);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 2);

        CHECK(diagnostics[0].code == "E_TYPEDEF_ONLY_PRIMITIVE");
        CHECK(diagnostics[0].range.start.line == 4);
        CHECK(diagnostics[0].range.start.character == 20); // Span over 'string'

        CHECK(diagnostics[1].code == "E_TYPEDEF_ONLY_PRIMITIVE");
        CHECK(diagnostics[1].range.start.line == 5);
        CHECK(diagnostics[1].range.start.character == 20); // Span over 'Vector3'
    }

    TEST_CASE("Invalid Typedefs: Reject handles and templates in standard script files")
    {
        const char *script = R"(
            class MyClass {}

            typedef MyClass@ ClassHandle; // Error: Handle typedef disallowed
        )";

        auto doc = CreateTestDocument("file:///workspace/main.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "E_TYPEDEF_ONLY_PRIMITIVE");
        CHECK(diagnostics[0].range.start.line == 3);
    }
}

TEST_SUITE("AngelScript_Funcdef_Verification")
{
    TEST_CASE("Predefined & Script: Register funcdef, bind matching function, and deduce return type")
    {
        const char *predefinedScript = R"(
            funcdef bool Predicate(int value);
            funcdef void Action();
        )";

        const char *userScript = R"(
            bool IsEven(int val) { return (val % 2) == 0; }
            void DoWork() {}

            void Main() {
                Predicate@ pred = @IsEven; // OK: Matching signature
                Action@ act = @DoWork;      // OK: Matching signature

                auto result = pred(42);     // Deduces bool
                act();                      // Invokes void Action()
            }
        )";

        auto predefinedDoc = CreateTestDocument("file:///workspace/as.predefined", predefinedScript);
        auto userDoc = CreateTestDocument("file:///workspace/main.as", userScript);
        REQUIRE(predefinedDoc != nullptr);
        REQUIRE(userDoc != nullptr);

        CHECK(predefinedDoc->GetDiagnostics().empty());
        CHECK(userDoc->GetDiagnostics().empty());

        // Validate inferred type for 'result' (bool)
        CHECK(userDoc->GetSymbolTypeAt({8, 22}) == "bool");

        // Validate resolved call on 'pred(42)'
        auto predCall = userDoc->GetResolvedCallAt({8, 31});
        REQUIRE(predCall.has_value());
        CHECK(predCall->targetFunctionSymbol == "Predicate::Predicate(int)");
    }

    TEST_CASE("Diagnostics: Incompatible function signature bound to funcdef handle")
    {
        const char *script = R"(
            funcdef int Operation(int a, int b);

            void IncompatibleReturn(int a, int b) {}
            int IncompatibleParams(string a, int b) { return 0; }

            void Main() {
                Operation@ op1 = @IncompatibleReturn; // Error: void vs int return
                Operation@ op2 = @IncompatibleParams; // Error: string vs int param
            }
        )";

        auto doc = CreateTestDocument("file:///test_funcdef_mismatch.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 2);

        CHECK(diagnostics[0].code == "E_SIGNATURE_MISMATCH_FUNC_HANDLE");
        CHECK(diagnostics[0].range.start.line == 7);

        CHECK(diagnostics[1].code == "E_SIGNATURE_MISMATCH_FUNC_HANDLE");
        CHECK(diagnostics[1].range.start.line == 8);
    }

    TEST_CASE("Diagnostics: Duplicate funcdef declarations in as.predefined")
    {
        const char *predefinedScript = R"(
            funcdef void EventCallback(float deltaTime);
            funcdef void EventCallback(float deltaTime); // Error: Duplicate funcdef
        )";

        auto doc = CreateTestDocument("file:///workspace/as.predefined", predefinedScript);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "E_DUPLICATE_DECLARATION");
        CHECK(diagnostics[0].range.start.line == 2);
    }

    TEST_CASE("Diagnostics: Unknown types used in funcdef signature")
    {
        const char *predefinedScript = R"(
            funcdef UnregisteredType BadFuncDef(UnknownParam p);
        )";

        auto doc = CreateTestDocument("file:///workspace/as.predefined", predefinedScript);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 2);
        CHECK(diagnostics[0].code == "E_UNKNOWN_TYPE"); // UnregisteredType
        CHECK(diagnostics[1].code == "E_UNKNOWN_TYPE"); // UnknownParam
    }
}

TEST_SUITE("AngelScript_EngineParity_Verification")
{
    TEST_CASE("Const Invariant: Non-const method called through const handle/reference")
    {
        // --- GROUND TRUTH ORACLE (asharness.exe) ---
        // Snippet:
        //   class Entity { int v; void Mutate() { v = 1; } }
        //   void Take(const Entity &in e) { e.Mutate(); }
        // Engine Verdict:
        //   [ERROR] (2, 35) : No matching signatures to 'Entity::Mutate() const'
        // -------------------------------------------

        const char *script = R"(
            class Entity {
                int v;
                void Mutate() { v = 1; }
            }

            void Take(const Entity &in e) {
                e.Mutate(); // Must fail parity check
            }
        )";

        auto doc = CreateTestDocument("file:///test_const_parity.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);

        // Assert exact diagnostic parity
        CHECK(diagnostics[0].code == "as-err-const-method-required");
        CHECK(diagnostics[0].range.start.line == 7);
        CHECK(diagnostics[0].range.start.character == 18); // Exact span over 'Mutate'
    }

    TEST_CASE("Operator Parity: Dual-dispatch precedence vs explicit conversion")
    {
        // --- GROUND TRUTH ORACLE (asharness.exe) ---
        // Snippet: Matrix * Vector with opMul and opMul_r
        // Engine Verdict: Successfully resolves to Vector::opMul_r with 0 errors
        // -------------------------------------------

        const char *script = R"(
            class Matrix {
                Matrix opMul(float s) const { return Matrix(); }
            }
            class Vector {
                Vector opMul_r(const Matrix &in m) const { return Vector(); }
            }

            void Main() {
                Matrix m;
                Vector v;
                Vector res = m * v; // Engine accepts via opMul_r
            }
        )";

        auto doc = CreateTestDocument("file:///test_op_mul_parity.as", script);
        REQUIRE(doc != nullptr);

        // Ground truth: 0 diagnostics emitted by official compiler
        CHECK(doc->GetDiagnostics().empty());

        auto resolvedCall = doc->GetResolvedCallAt({11, 31});
        REQUIRE(resolvedCall.has_value());
        CHECK(resolvedCall->targetFunctionSymbol == "Vector::opMul_r(const Matrix &in) const");
    }
}

TEST_SUITE("AngelScript_MixinClasses_Verification")
{
    TEST_CASE("Instantiation: Reject direct instantiation or handles of mixin classes")
    {
        const char *script = R"(
            mixin class HelperMixin {
                void Help() {}
            }

            void Main() {
                HelperMixin m;                // Error: Cannot instantiate mixin
                HelperMixin@ h = HelperMixin(); // Error: Cannot create handle to mixin
            }
        )";

        auto doc = CreateTestDocument("file:///test_mixin_instantiate.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 2);

        CHECK(diagnostics[0].code == "E_CANNOT_INSTANTIATE_MIXIN");
        CHECK(diagnostics[0].range.start.line == 6);

        CHECK(diagnostics[1].code == "E_CANNOT_INSTANTIATE_MIXIN");
        CHECK(diagnostics[1].range.start.line == 7);
    }

    TEST_CASE("Method Precedence: Mixin overrides base class method, class overrides mixin")
    {
        const char *script = R"(
            class Base {
                string GetName() const { return "Base"; }
            }

            mixin class MixinA {
                string GetName() const { return "MixinA"; }
                string GetTag() const { return "TagA"; }
            }

            // Case 1: Mixin overrides Base::GetName
            class DerivedA : Base, MixinA {}

            // Case 2: DerivedB overrides MixinA::GetTag
            class DerivedB : MixinA {
                string GetTag() const { return "DerivedB"; }
            }

            void Main() {
                DerivedA da;
                auto name = da.GetName(); // Resolves to MixinA::GetName

                DerivedB db;
                auto tag = db.GetTag();   // Resolves to DerivedB::GetTag
            }
        )";

        auto doc = CreateTestDocument("file:///test_mixin_precedence.as", script);
        REQUIRE(doc != nullptr);
        CHECK(doc->GetDiagnostics().empty());

        auto callName = doc->GetResolvedCallAt({20, 29});
        REQUIRE(callName.has_value());
        CHECK(callName->targetFunctionSymbol == "MixinA::GetName() const");

        auto callTag = doc->GetResolvedCallAt({23, 28});
        REQUIRE(callTag.has_value());
        CHECK(callTag->targetFunctionSymbol == "DerivedB::GetTag() const");
    }

    TEST_CASE("Property Precedence: Base class property shadows mixin property")
    {
        const char *script = R"(
            class Base {
                int score;
            }

            mixin class MixinScore {
                float score; // Should not overwrite Base::score (int)
            }

            class Player : Base, MixinScore {}

            void Main() {
                Player p;
                auto s = p.score; // Must deduce 'int' from Base, not 'float'
            }
        )";

        auto doc = CreateTestDocument("file:///test_mixin_property.as", script);
        REQUIRE(doc != nullptr);
        CHECK(doc->GetDiagnostics().empty());

        // Validate that p.score resolves to int
        CHECK(doc->GetSymbolTypeAt({13, 22}) == "int");
    }

    TEST_CASE("Deferred Context: Mixin method referencing target class members")
    {
        const char *script = R"(
            mixin class StateMixin {
                void Increment() {
                    count++; // 'count' is declared in the target class
                }
            }

            class Counter : StateMixin {
                int count = 0;
            }

            void Main() {
                Counter c;
                c.Increment(); // Valid: resolved in Counter scope
            }
        )";

        auto doc = CreateTestDocument("file:///test_mixin_context.as", script);
        REQUIRE(doc != nullptr);
        CHECK(doc->GetDiagnostics().empty());
    }

    TEST_CASE("Interfaces: Mixin implements partial interface, class implements remainder")
    {
        const char *script = R"(
            interface IService {
                void Start();
                void Stop();
            }

            mixin class ServiceMixin : IService {
                void Start() {} // Implements Start()
            }

            // Error: Missing Stop() implementation
            class IncompleteService : ServiceMixin {}

            // OK: Completes Stop() implementation
            class CompleteService : ServiceMixin {
                void Stop() {}
            }
        )";

        auto doc = CreateTestDocument("file:///test_mixin_interface.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);

        CHECK(diagnostics[0].code == "E_UNIMPLEMENTED_INTERFACE_METHOD");
        CHECK(diagnostics[0].range.start.line == 11); // IncompleteService
    }

    TEST_CASE("Inheritance Restriction: Reject mixin inheriting from class")
    {
        const char *script = R"(
            class PlainClass {}

            mixin class BadMixin : PlainClass {} // Error: Mixin cannot inherit from class
        )";

        auto doc = CreateTestDocument("file:///test_bad_mixin_inheritance.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "E_MIXIN_CANNOT_INHERIT_CLASS");
        CHECK(diagnostics[0].range.start.line == 3);
    }
}

TEST_SUITE("AngelScript_Namespaces_Verification") {

    TEST_CASE("Nested Namespaces: Scope resolution, shadowing, and global root operator (::)") {
        const char* script = R"(
            int var = 100;

            namespace Parent {
                int var = 200;

                namespace Child {
                    int var = 300;

                    void TestScopes() {
                        int local = var;          // Resolves to Child::var (300)
                        int pVal  = Parent::var;  // Resolves to Parent::var (200)
                        int gVal  = ::var;        // Resolves to global ::var (100)
                    }
                }
            }

            void Main() {
                int fromChild = Parent::Child::var; // Resolves to 300
            }
        )";

        auto doc = CreateTestDocument("file:///test_namespace_scopes.as", script);
        REQUIRE(doc != nullptr);
        CHECK(doc->GetDiagnostics().empty());

        // Validate resolved symbol targets
        auto localCall = doc->GetSymbolDefinitionAt({10, 37}); // 'var' in local = var
        REQUIRE(localCall.has_value());
        CHECK(localCall->targetSymbol == "Parent::Child::var");

        auto parentCall = doc->GetSymbolDefinitionAt({11, 44}); // 'var' in Parent::var
        REQUIRE(parentCall.has_value());
        CHECK(parentCall->targetSymbol == "Parent::var");

        auto globalCall = doc->GetSymbolDefinitionAt({12, 39}); // 'var' in ::var
        REQUIRE(globalCall.has_value());
        CHECK(globalCall->targetSymbol == "::var");
    }

    TEST_CASE("Using Namespace: Unqualified symbol lookup across namespaces") {
        const char* script = R"(
            namespace Math {
                float ComputeSin(float rad) { return 0.0f; }
            }

            using namespace Math;

            void Main() {
                auto val = ComputeSin(3.14159f); // Resolves to Math::ComputeSin
            }
        )";

        auto doc = CreateTestDocument("file:///test_using_namespace.as", script);
        REQUIRE(doc != nullptr);
        CHECK(doc->GetDiagnostics().empty());

        auto call = doc->GetResolvedCallAt({8, 30});
        REQUIRE(call.has_value());
        CHECK(call->targetFunctionSymbol == "Math::ComputeSin(float)");
    }

    TEST_CASE("Diagnostics: Ambiguous symbol call from multiple using namespace directives") {
        const char* script = R"(
            namespace PackageA {
                void Initialize() {}
            }

            namespace PackageB {
                void Initialize() {}
            }

            using namespace PackageA;
            using namespace PackageB;

            void Main() {
                Initialize(); // Error: Ambiguous match between PackageA and PackageB
            }
        )";

        auto doc = CreateTestDocument("file:///test_namespace_ambiguity.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "E_AMBIGUOUS_IDENTIFIER");
        CHECK(diagnostics[0].range.start.line == 13);
    }

    TEST_CASE("Diagnostics: Undefined namespace in qualified path") {
        const char* script = R"(
            namespace Geometry {
                int sides = 4;
            }

            void Main() {
                int invalid = UnknownSpace::sides; // Error: Unknown namespace
            }
        )";

        auto doc = CreateTestDocument("file:///test_undefined_namespace.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "E_UNDEFINED_NAMESPACE");
        CHECK(diagnostics[0].range.start.line == 6);
        CHECK(diagnostics[0].range.start.character == 30); // Span over 'UnknownSpace'
    }
}

TEST_SUITE("AngelScript_Imports_Verification") {

    TEST_CASE("Imports: Declaration, call resolution, and hover module provenance") {
        const char* script = R"(import void ExternalLog(int level, const string &in message) from "LogModule";
import float CalculateBonus(float base) from "EconomyModule";

void Main() {
    ExternalLog(1, "System initialized"); // Valid call to imported function
    auto bonus = CalculateBonus(100.0f);   // Deduces float
})";

        auto doc = CreateTestDocument("file:///test_imports_valid.as", script);
        REQUIRE(doc != nullptr);
        CHECK(doc->GetDiagnostics().empty());

        // 1. Validate inferred return type for 'bonus'
        CHECK(doc->GetSymbolTypeAt({5, 9}) == "float");

        // 2. Validate resolved call target
        auto logCall = doc->GetResolvedCallAt({4, 10});
        REQUIRE(logCall.has_value());
        CHECK(logCall->targetFunctionSymbol == "ExternalLog(int, const string &in)");

        // 3. Validate hover displaying import syntax and module origin
        auto hover = doc->GetHoverAt({4, 10});
        REQUIRE(hover.has_value());
        CHECK(hover->contents.value.find("import void ExternalLog(int level, const string &in message) from \"LogModule\"") != std::string::npos);
    }

    TEST_CASE("Diagnostics: Reject import declarations containing function bodies") {
        const char* script = R"(
            import void InvalidImport() from "Module" {
                // Error: Imports cannot have implementations
            }
        )";

        auto doc = CreateTestDocument("file:///test_import_with_body.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "E_IMPORT_CANNOT_HAVE_BODY");
        CHECK(diagnostics[0].range.start.line == 1);
    }

    TEST_CASE("Diagnostics: Duplicate import declarations") {
        const char* script = R"(
            import void ServiceTick(float dt) from "Engine";
            import void ServiceTick(float dt) from "Engine"; // Error: Duplicate import
        )";

        auto doc = CreateTestDocument("file:///test_duplicate_import.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "E_DUPLICATE_DECLARATION");
        CHECK(diagnostics[0].range.start.line == 2);
    }

    TEST_CASE("Diagnostics: Unregistered types in import signature") {
        const char* script = R"(
            import UnregisteredReturn QueryData(UnregisteredParam p) from "DataModule";
        )";

        auto doc = CreateTestDocument("file:///test_import_unknown_types.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 2);
        CHECK(diagnostics[0].code == "E_UNKNOWN_TYPE"); // UnregisteredReturn
        CHECK(diagnostics[1].code == "E_UNKNOWN_TYPE"); // UnregisteredParam
    }
}

TEST_SUITE("AngelScript_Statements_Verification") {

    TEST_CASE("Variable Declarations: Comma chaining, initialization, and sub-block shadowing") {
        const char* script = R"(
            void Main() {
                int a = 1, b = 2;
                float pi = 3.14159f;

                {
                    float a = 10.5f; // Valid sub-block shadowing
                    float innerResult = a + pi; // 'a' is float here
                }

                int outerResult = a + b; // 'a' is int (1) here
            }
        )";

        auto doc = CreateTestDocument("file:///test_var_declarations.as", script);
        REQUIRE(doc != nullptr);
        CHECK(doc->GetDiagnostics().empty());

        // Validate symbol types across scopes
        CHECK(doc->GetSymbolTypeAt({6, 27}) == "float"); // Inner 'a'
        CHECK(doc->GetSymbolTypeAt({10, 35}) == "int");   // Outer 'a'
    }

    TEST_CASE("Switch-Case: Constant folding, non-constexpr detection, and duplicate cases") {
        const char* script = R"(
            const int VALID_CONST = 10;
            int runtimeVar = 20;

            void Process(int val) {
                switch (val) {
                    case 0:
                        break;
                    case VALID_CONST: // OK: Compile-time constant
                        break;
                    case 0:           // Error: Duplicate case value
                        break;
                    case runtimeVar:  // Error: Non-constant expression
                        break;
                    default:
                        break;
                }
            }
        )";

        auto doc = CreateTestDocument("file:///test_switch_cases.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 2);

        CHECK(diagnostics[0].code == "E_DUPLICATE_CASE");
        CHECK(diagnostics[0].range.start.line == 10);

        CHECK(diagnostics[1].code == "E_NOT_A_CONSTANT_EXPRESSION");
        CHECK(diagnostics[1].range.start.line == 12);
    }

    TEST_CASE("Loop Control: Break and continue scope boundaries") {
        const char* script = R"(
            void Main() {
                while (true) {
                    break;    // OK
                    continue; // OK
                }

                switch (1) {
                    case 1:
                        break;    // OK: Inside switch
                        continue; // Error: Continue is not valid directly in switch
                }

                break;    // Error: Break outside loop/switch
                continue; // Error: Continue outside loop
            }
        )";

        auto doc = CreateTestDocument("file:///test_loop_control.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 3);

        CHECK(diagnostics[0].code == "E_CONTINUE_OUTSIDE_LOOP");
        CHECK(diagnostics[0].range.start.line == 10);

        CHECK(diagnostics[1].code == "E_BREAK_OUTSIDE_LOOP");
        CHECK(diagnostics[1].range.start.line == 13);

        CHECK(diagnostics[2].code == "E_CONTINUE_OUTSIDE_LOOP");
        CHECK(diagnostics[2].range.start.line == 14);
    }

    TEST_CASE("Return Semantics: Void returning expression vs Non-void missing/invalid return") {
        const char* script = R"(
            void VoidFunc() {
                return 42; // Error: Void function cannot return a value
            }

            int IntFunc() {
                return "text"; // Error: Type mismatch (string to int)
            }
        )";

        auto doc = CreateTestDocument("file:///test_return_semantics.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 2);

        CHECK(diagnostics[0].code == "E_VOID_CANNOT_RETURN_VALUE");
        CHECK(diagnostics[0].range.start.line == 2);

        CHECK(diagnostics[1].code == "E_RETURN_TYPE_MISMATCH");
        CHECK(diagnostics[1].range.start.line == 6);
    }

    TEST_CASE("Scoped Using Namespace: Block lifetime and isolation") {
        const char* script = R"(
            namespace HiddenMath {
                void FastSqrt() {}
            }

            void Main() {
                {
                    using namespace HiddenMath;
                    FastSqrt(); // OK: Visible inside block
                }

                FastSqrt(); // Error: HiddenMath is no longer in scope
            }
        )";

        auto doc = CreateTestDocument("file:///test_scoped_using.as", script);
        REQUIRE(doc != nullptr);

        auto diagnostics = doc->GetDiagnostics();
        REQUIRE(diagnostics.size() == 1);

        CHECK(diagnostics[0].code == "E_UNDEFINED_IDENTIFIER");
        CHECK(diagnostics[0].range.start.line == 11);
    }
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Type Rules Corpus Audit*"`)
// =====================================================================================

TEST_CASE("TypeRules - Type Rules Corpus Audit" * doctest::skip(true))
{
    static const std::vector<std::string> k_codes = {
        "as-err-typedef-non-primitive", "as-err-enum-invalid-initializer",
        "as-err-interface-constructor", "as-err-duplicate-symbol", "as-err-name-conflict",
        "as-err-handle-on-primitive", "as-syntax-error-missing",
        "as-err-declaration-missing-body", "as-err-external-not-shared",
        "as-err-funcdef-attribute"
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
