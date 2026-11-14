// @expect: accept
int f(int x) {
    switch (x) {
        case 1:
        case 2:
            return 12;
        default:
            return 0;
    }
}
