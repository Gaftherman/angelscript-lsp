// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(6, 15, "Multiple matching signatures to 'foo()'")]
// @expect: reject
namespace A { void foo() {} }
namespace B { void foo() {} }
using namespace A;
using namespace B;
void main() { foo(); }
