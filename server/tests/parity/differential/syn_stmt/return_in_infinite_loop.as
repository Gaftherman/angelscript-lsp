// @expect: reject
int f() {
    while (true) {
        return 1;
    }
}
