// @expect: accept
// @complete+ onlyA
// @complete- onlyB
namespace A { void onlyA() {} }
namespace B { void onlyB() {} }
void f() {
    A::only/*$complete*/();
}
