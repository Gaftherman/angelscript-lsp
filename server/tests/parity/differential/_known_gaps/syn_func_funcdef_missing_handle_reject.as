// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 18, "Data type can't be 'Op'")]
// @expect: reject
funcdef int Op(int);
int inc(int a) { return a + 1; }
void main() { Op fn = inc; }
