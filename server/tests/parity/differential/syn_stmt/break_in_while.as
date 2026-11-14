// @expect: accept
int f() {
    int i = 0;
    while (true) {
        if (i == 5) break;
        i++;
    }
    return i;
}
