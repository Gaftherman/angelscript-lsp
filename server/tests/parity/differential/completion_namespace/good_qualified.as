// @expect: accept
// @complete+ Clap
namespace A { namespace B { class Clap {} class Zed {} } }
void f() { A::B::Cl/*$complete*/ x; }
