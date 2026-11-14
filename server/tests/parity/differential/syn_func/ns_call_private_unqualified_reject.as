// @expect: reject
namespace A { int f() { return 1; } }
void main() { int x = f(); }
