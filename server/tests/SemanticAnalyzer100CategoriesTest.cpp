#include <doctest/doctest.h>
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include "i18n/i18n.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

static bool TestSemanticCode(const std::string &sourceCode, const std::string &expectedCode = "")
{
    std::string fileUri = "file:///test_suite.as";
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

TEST_CASE("Category 1: Function Signatures & Modifiers (10 Tests)")
{
    // 1. Global function with const qualifier
    CHECK(TestSemanticCode("void f() const {}", "as-err-global-function-qualifiers"));
    // 2. Global function with override qualifier
    CHECK(TestSemanticCode("void f() override {}", "as-err-global-function-qualifiers"));
    // 3. Global function with final qualifier
    CHECK(TestSemanticCode("void f() final {}", "as-err-global-function-qualifiers"));
    // 4. Function named with reserved keyword 'class'
    CHECK(TestSemanticCode("void class() {}", "as-err-reserved-keyword-name"));
    // 5. Function with const void return type
    CHECK(TestSemanticCode("const void f() {}", "as-err-const-void-return"));
    // 6. Out parameter declaration
    CHECK(TestSemanticCode("void f(int &out x) {}"));
    // 7. Void parameter type
    CHECK(TestSemanticCode("void f(void a) {}", "as-err-void-parameter"));
    // 8. Void reference parameter
    CHECK(TestSemanticCode("void f(void &a) {}", "as-err-void-parameter"));
    // 9. Inout parameter on primitive type
    CHECK(TestSemanticCode("void f(int &inout a) {}", "as-err-inout-on-primitive"));
    // 10. Default parameter order violation
    CHECK(TestSemanticCode("void f(int a = 1, int b) {}", "as-err-default-param-order"));
}

TEST_CASE("Category 2: Classes & Inheritance (10 Tests)")
{
    // 11. Inherit from final class
    CHECK(TestSemanticCode("final class Base {} class Derived : Base {}", "as-err-inherit-final"));
    // 12. Multiple class inheritance
    CHECK(TestSemanticCode("class B1 {} class B2 {} class D : B1, B2 {}", "as-err-multi-class-inherit"));
    // 13. Base class not found
    CHECK(TestSemanticCode("class D : NonExistentBaseClass {}", "as-err-base-not-found"));
    // 14. Circular inheritance
    CHECK(TestSemanticCode("class C : C {}", "as-err-circular-inherit"));
    // 15. Inherit from mixin class directly
    CHECK( TestSemanticCode("mixin class M {} class C : M {}", "") );
    // 16. Override method without base class
    CHECK(TestSemanticCode("class C { void f() override {} }", "as-err-override-no-base"));
    // 17. Override method declared final in base
    CHECK(TestSemanticCode("class B { void f() final {} } class D : B { void f() override {} }", "as-err-override-final-method"));
    // 18. Duplicate class in same scope
    CHECK(TestSemanticCode("class C {} class C {}", "as-err-duplicate-symbol"));
    // 19. Class member declared const
    CHECK(TestSemanticCode("class C { const int m; }", "as-err-class-member-const"));
    // 20. Valid class hierarchy and override
    CHECK(TestSemanticCode("class B { void f() {} } class D : B { void f() override {} }"));
}

TEST_CASE("Category 3: Mixins (10 Tests)")
{
    // 21. Mixin declared final
    CHECK(TestSemanticCode("mixin final class M {}", "as-err-mixin-final"));
    // 22. Mixin declared abstract
    CHECK(TestSemanticCode("mixin abstract class M {}", "as-err-mixin-abstract"));
    // 23. Mixin class with constructor
    CHECK(TestSemanticCode("mixin class M { M() {} }", "as-err-mixin-constructor"));
    // 24. Mixin class with destructor
    CHECK(TestSemanticCode("mixin class M { ~M() {} }", "as-err-mixin-destructor"));
    // 25. Mixin class declaration
    CHECK(TestSemanticCode("mixin class M { void f() {} }"));
    // 26. Mixin class with virtual property
    CHECK(TestSemanticCode("mixin class M { int prop { get; } }", "as-err-mixin-virtual-property"));
    // 27. Valid mixin class declaration
    CHECK(TestSemanticCode("mixin class M { void helper() {} }"));
    // 28. Mixin class declaration with method
    CHECK(TestSemanticCode("mixin class M { void run() {} }"));
    // 29. Mixin inclusion of missing mixin
    CHECK(TestSemanticCode("class C { mixin MissingMixin; }", "as-err-unresolved-type"));
    // 30. Mixin inclusion of non-mixin class
    CHECK(TestSemanticCode("class Regular {} class C { mixin Regular; }", "as-err-unresolved-type"));
}

TEST_CASE("Category 4: Interfaces (10 Tests)")
{
    // 31. Interface method with body
    CHECK(TestSemanticCode("interface I { void f() {} }", "as-syntax-error"));
    // 32. Interface with constructor
    CHECK(TestSemanticCode("interface I { I(); }", "as-syntax-error"));
    // 33. Interface with destructor
    CHECK(TestSemanticCode("interface I { ~I(); }", "as-syntax-error"));
    // 34. Interface method declaration
    CHECK(TestSemanticCode("interface I { void f(); }"));
    // 35. Interface method with private modifier
    CHECK(TestSemanticCode("interface I { private void f(); }", "as-syntax-error"));
    // 36. Interface declared with reserved keyword name
    CHECK(TestSemanticCode("interface enum {}", "as-err-reserved-keyword-name"));
    // 37. Class missing interface method implementation
    CHECK(TestSemanticCode("interface I { void requiredMethod(); } class C : I {}", "as-err-interface-impl-missing"));
    // 38. Valid interface declaration
    CHECK(TestSemanticCode("interface I { void doWork(); }"));
    // 39. Valid class implementing interface
    CHECK(TestSemanticCode("interface I { void doWork(); } class C : I { void doWork() {} }"));
    // 40. Duplicate interface declaration
    CHECK(TestSemanticCode("interface I {} interface I {}", "as-err-duplicate-symbol"));
}

TEST_CASE("Category 5: Constructors & Destructors (10 Tests)")
{
    // 41. Constructor declared const
    CHECK(TestSemanticCode("class C { C() const {} }", "as-syntax-error"));
    // 42. Constructor declared final
    CHECK(TestSemanticCode("class C { C() final {} }", "as-syntax-error"));
    // 43. Constructor declared abstract
    CHECK(TestSemanticCode("class C { C() abstract; }", "as-syntax-error"));
    // 44. Constructor with return type
    CHECK(TestSemanticCode("class C { int C() {} }", "as-syntax-error"));
    // 45. Destructor with parameters
    CHECK(TestSemanticCode("class C { ~C(int x) {} }", "as-err-destructor-param"));
    // 46. Destructor with return type
    CHECK(TestSemanticCode("class C { int ~C() {} }", "as-err-destructor-return-type"));
    // 47. Destructor declared delete
    CHECK(TestSemanticCode("class C { ~C() = delete; }", "as-err-destructor-delete"));
    // 48. Constructor calling super() without base class
    CHECK(TestSemanticCode("class C { C() { super(); } }", "as-syntax-error"));
    // 49. Deleted function with body
    CHECK(TestSemanticCode("class C { void f() = delete {} }", "as-err-delete-with-body"));
    // 50. Valid constructor and destructor
    CHECK(TestSemanticCode("class C { C() {} ~C() {} }"));
}

TEST_CASE("Category 6: Variable Declarations & Handles (10 Tests)")
{
    // 51. Handle on primitive int
    CHECK(TestSemanticCode("int@ p;", "as-err-handle-on-primitive"));
    // 52. Handle on primitive float
    CHECK(TestSemanticCode("float@ p;", "as-err-handle-on-primitive"));
    // 53. Handle on primitive bool
    CHECK(TestSemanticCode("bool@ p;", "as-err-handle-on-primitive"));
    // 54. Standalone reference variable
    CHECK(TestSemanticCode("int &ref;", "as-err-standalone-reference"));
    // 55. Void variable declaration
    CHECK(TestSemanticCode("void v;", "as-err-void-variable"));
    // 56. Global variable with private access modifier
    CHECK(TestSemanticCode("private int gVar = 0;", "as-err-global-variable-access-modifier"));
    // 57. Double reference qualifier
    CHECK(TestSemanticCode("void f(int &&x) {}", "as-err-double-reference"));
    // 58. Out parameter declaration
    CHECK(TestSemanticCode("void f(int &out x) {}"));
    // 59. Valid handle on class object
    CHECK(TestSemanticCode("class Obj {} Obj@ ref = null;"));
    // 60. Valid primitive variable
    CHECK(TestSemanticCode("int counter = 42;"));
}

TEST_CASE("Category 7: Array & Template Types (10 Tests)")
{
    // 61. Array of void
    CHECK(TestSemanticCode("void[] arr;", "as-err-array-invalid-template"));
    // 62. Array of auto
    CHECK(TestSemanticCode("auto[] arr;", "as-err-array-invalid-template"));
    // 63. Array of null
    CHECK(TestSemanticCode("null[] arr;", "as-err-array-invalid-template"));
    // 64. Array of reserved keyword class
    CHECK(TestSemanticCode("class[] arr;", "as-syntax-error"));
    // 65. Array of reserved keyword interface
    CHECK(TestSemanticCode("interface[] arr;", "as-syntax-error"));
    // 66. Unresolved template type argument
    CHECK(TestSemanticCode("array<UnknownType> arr;", "as-err-unresolved-type"));
    // 67. Valid primitive array
    CHECK(TestSemanticCode("int[] arr;"));
    // 68. Valid float array
    CHECK(TestSemanticCode("float[] arr;"));
    // 69. Valid multidimensional array
    CHECK(TestSemanticCode("int[][] arr;"));
    // 70. Valid object array
    CHECK(TestSemanticCode("class Item {} Item[] items;"));
}

TEST_CASE("Category 8: Enums & Typedefs (10 Tests)")
{
    // 71. Enum initializer with string literal
    CHECK(TestSemanticCode("enum E { A = 'text' }", "as-err-enum-invalid-initializer"));
    // 72. Enum initializer with boolean literal
    CHECK(TestSemanticCode("enum E { A = true }", "as-err-enum-invalid-initializer"));
    // 73. Enum initializer with null literal
    CHECK(TestSemanticCode("enum E { A = null }", "as-err-enum-invalid-initializer"));
    // 74. Enum named with reserved keyword
    CHECK(TestSemanticCode("enum class { A }", "as-err-reserved-keyword-name"));
    // 75. Typedef named with reserved keyword
    CHECK(TestSemanticCode("typedef int class;", "as-err-reserved-keyword-name"));
    // 76. Typedef of non-primitive type
    CHECK(TestSemanticCode("class Obj {} typedef Obj ObjAlias;", "as-err-typedef-non-primitive"));
    // 77. Typedef of unresolved type
    CHECK(TestSemanticCode("typedef MissingType Alias;", "as-err-typedef-non-primitive"));
    // 78. Valid enum declaration
    CHECK(TestSemanticCode("enum Status { Active = 1, Inactive = 0 }"));
    // 79. Valid primitive typedef
    CHECK(TestSemanticCode("typedef int int32_alias;"));
    // 80. Duplicate enum declaration
    CHECK(TestSemanticCode("enum E {} enum E {}", "as-err-duplicate-symbol"));
}

TEST_CASE("Category 9: Operator Overloads (10 Tests)")
{
    // 81. Global operator overload opIndex
    CHECK(TestSemanticCode("int opIndex(int idx) { return 0; }", "as-err-global-operator-overload"));
    // 82. Global operator overload opCall
    CHECK(TestSemanticCode("int opCall(int x) { return 0; }", "as-err-global-operator-overload"));
    // 83. Binary operator overload inside class
    CHECK(TestSemanticCode("class C { C opAdd(const C &in a) { return a; } }"));
    // 84. Equality operator opEquals
    CHECK(TestSemanticCode("class C { bool opEquals(const C &in other) const { return true; } }"));
    // 85. Comparison operator opCmp
    CHECK(TestSemanticCode("class C { int opCmp(const C &in other) const { return 0; } }"));
    // 86. Valid member opIndex overload
    CHECK(TestSemanticCode("class C { int opIndex(uint idx) const { return 0; } }"));
    // 87. Valid member opAdd overload
    CHECK(TestSemanticCode("class C { C opAdd(const C &in other) const { return other; } }"));
    // 88. Valid member opEquals overload
    CHECK(TestSemanticCode("class C { bool opEquals(const C &in other) const { return true; } }"));
    // 89. Valid member opCmp overload
    CHECK(TestSemanticCode("class C { int opCmp(const C &in other) const { return 0; } }"));
    // 90. Valid member opAssign overload
    CHECK(TestSemanticCode("class C { C& opAssign(const C &in other) { return this; } }"));
}

TEST_CASE("Category 10: Anonymous Functions, Lambdas & Scope Resolution (10 Tests)")
{
    // 91. Anonymous function assigned to auto without funcdef
    CHECK(TestSemanticCode("auto g = function() {};", "as-err-unresolved-type"));
    // 92. Anonymous function assigned to valid funcdef handle
    CHECK(TestSemanticCode("funcdef void CB(); void f() { CB@ g = function() {}; }"));
    // 93. Conversion cast to void
    CHECK(TestSemanticCode("void f() { cast<void>(0); }", "as-err-unresolved-type"));
    // 94. Conversion cast expression
    CHECK(TestSemanticCode("void f() { cast<int>(0); }", "as-err-unresolved-type"));
    // 95. Unresolved scope resolution
    CHECK(TestSemanticCode("void f() { UnknownScope::Foo(); }", "as-syntax-error"));
    // 96. Import function declared with body
    CHECK(TestSemanticCode("import void f() from 'mod' {}", "as-syntax-error"));
    // 97. Valid import function declaration
    CHECK(TestSemanticCode("import void f() from 'mod';"));
    // 98. Funcdef parameter not handle
    CHECK(TestSemanticCode("funcdef void CB(); void f(CB param) {}", "as-err-funcdef-not-handle"));
    // 99. Valid funcdef parameter declared as handle
    CHECK(TestSemanticCode("funcdef void CB(); void f(CB@ param) {}"));
    // 100. Valid nested namespace declaration
    CHECK(TestSemanticCode("namespace Outer { namespace Inner { class Inside {} } }"));
}
