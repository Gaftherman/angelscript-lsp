// @expect: accept
namespace A::B { int num() { return 7; } }
using namespace A::B;
int x = num();
