// @expect: accept
int f() {
    int s = 0;
    for (int i = 0; i < 5; i++) {
        if (i == 2) continue;
        s += i;
    }
    return s;
}
