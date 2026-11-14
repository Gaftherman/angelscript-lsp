// @expect: accept
int f(int x) {
    int r = 0;
    switch (x) {
        case 1:
            r = 10;
            break;
        case 2:
            r = 20;
            break;
        default:
            r = -1;
            break;
    }
    return r;
}
