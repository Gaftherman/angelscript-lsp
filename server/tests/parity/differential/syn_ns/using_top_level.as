// @expect: accept
namespace A { void foo() {} }
using namespace A;
void main() { foo(); }
