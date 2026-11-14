// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(3, 5, 'Empty switch statement')]
// @expect: reject
int f(int x) {
    switch (x) {
        default:
            return 42;
    }
}
