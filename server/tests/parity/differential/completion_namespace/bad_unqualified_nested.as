// @expect: reject
// @complete- Clap
namespace A { namespace B { class Clap {} } }
void f() { Cl/*$complete*/ x; }
