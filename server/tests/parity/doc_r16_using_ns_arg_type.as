// A using-directive puts a name in reach, so a call through one is judged like any other:
//     ERROR (3, 24): No matching signatures to 'f(int)'
namespace A { void f(string s) {} }
using namespace A;
void g() { int id = 1; f(id); }
