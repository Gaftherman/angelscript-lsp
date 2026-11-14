// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 14, 'Case expressions must be literal constants')]
// @expect: reject
int f(int x, int y) {
    switch (x) {
        case y: return 1;
        default: return 0;
    }
}
