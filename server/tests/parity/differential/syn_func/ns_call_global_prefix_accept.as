// @expect: accept
int g() { return 1; }
namespace A { int g() { return 2; } int test() { return ::g(); } }
void main() { int x = A::test(); }
