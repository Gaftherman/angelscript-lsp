// @expect: reject
void f(int x) {
    if (x) {
        x = 0;
    }
}
