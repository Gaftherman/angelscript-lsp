// @expect: accept
namespace A { namespace B { int f() { return 1; } } }
void main() { int x = A::B::f(); }
