// @expect: reject
namespace A { void f() {} }
using namespace A
void main() {}
