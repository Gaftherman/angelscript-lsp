// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 15, "Multiple matching signatures to 'f(const int)'")]
// @expect: reject
void f(float a) {}
void f(double a) {}
void main() { f(1); }
