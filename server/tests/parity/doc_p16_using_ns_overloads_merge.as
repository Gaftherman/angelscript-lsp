// Two using-directives contribute at once - a directive does not shadow and does not stop the
// search - so ordinary overload resolution picks B::f(int) here. With both taking a string the
// compiler answers "Multiple matching signatures" instead, which is why the sets are merged
// and the ambiguity is left to overload resolution rather than special-cased.
namespace A { void f(string s) {} }
namespace B { void f(int i) {} }
using namespace A;
using namespace B;
void g() { f(1); }
