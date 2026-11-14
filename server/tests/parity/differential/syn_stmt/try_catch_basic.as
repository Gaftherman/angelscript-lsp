// @expect: accept
int f() {
    int x = 0;
    try {
        x = 1;
    } catch {
        x = -1;
    }
    return x;
}
