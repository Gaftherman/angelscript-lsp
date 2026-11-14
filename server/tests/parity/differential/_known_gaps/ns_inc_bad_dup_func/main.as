// Known gap: the compiler rejects this include set; the server reports no error on main.as.
// @expect: reject
#include "a.as"
#include "b.as"
void main() {}
