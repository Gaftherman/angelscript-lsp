// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(3, 12, "No matching symbol 'A::C::f'")]
// @expect: reject
namespace A { namespace B { void f() {} } }
void g() { A::C::f(); }
