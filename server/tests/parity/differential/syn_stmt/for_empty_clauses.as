// @expect: accept
int f() {
    int i = 0;
    for (;;) {
        if (i >= 3) break;
        i++;
    }
    return i;
}
