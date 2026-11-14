// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(2, 8, 'Auto is not allowed here')]
// @expect: reject
void f(auto x) {}
