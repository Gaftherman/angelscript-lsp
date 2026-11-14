// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(3, 13, 'Switch expressions must be integral numbers')]
// @expect: reject
int f(float x) {
    switch (x) {
        case 1: return 1;
        default: return 0;
    }
}
