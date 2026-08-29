// A lexical scope shadows. `N::f(string)` hides the global `f(int)` from inside N, so the call
// does not fall through to it:
//     ERROR (2, 46): No matching signatures to 'f(const int)'
// This is why candidate collection stops at the first scope that declares the name.
void f(int i) {}
namespace N { void f(string s) {} void g() { f(1); } }
