// @expect: reject
int f(int x) {
    switch (x) {
        case 1: return 10;
        case 1: return 20;
        default: return 0;
    }
}
