// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 14, "Type 'int' doesn't support the indexing operator")]
// @expect: reject
void main() {
    int x = 5;
    int y = x[0];
}
