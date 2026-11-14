// @expect: reject
int f() {
    for (int i = 0; i < 3; i++) {
    }
    return i;
}
