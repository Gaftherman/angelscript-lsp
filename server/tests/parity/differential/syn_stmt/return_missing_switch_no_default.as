// @expect: reject
int f(int x) {
    switch (x) {
        case 1: return 10;
        case 2: return 20;
    }
}
