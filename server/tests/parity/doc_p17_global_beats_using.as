// The global scope declares `f`, so the search stops there and the using-directive is never
// reached. `f(1)` matches the global `f(int)`.
namespace A { void f(string s) {} }
void f(int i) {}
using namespace A;
void g() { f(1); }
