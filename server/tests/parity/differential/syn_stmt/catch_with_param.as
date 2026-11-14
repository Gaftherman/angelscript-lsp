// @expect: reject
void f() {
    try {
        int x = 1;
    } catch (int e) {
        int y = e;
    }
}
