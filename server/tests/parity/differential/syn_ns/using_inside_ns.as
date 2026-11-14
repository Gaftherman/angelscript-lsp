// @expect: accept
namespace A { void foo() {} }
namespace C {
    using namespace A;
    void bar() { foo(); }
}
