// Known gap: the compiler rejects this include set; the server reports no error on main.as.
// @expect: reject
#include "does_not_exist.as"
void main() {}
