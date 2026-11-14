// @expect: reject
void f(int x) {
    if (x = 1) {
        return;
    }
}
