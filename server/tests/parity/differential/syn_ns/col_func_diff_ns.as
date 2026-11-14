// @expect: accept
namespace A { void f(int a) {} }
namespace B { void f(float b) {} }
void main() { A::f(1); B::f(1.0f); }
