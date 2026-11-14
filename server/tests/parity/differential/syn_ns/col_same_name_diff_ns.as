// @expect: accept
namespace A { int x = 1; }
namespace B { int x = 2; }
int y = A::x + B::x;
