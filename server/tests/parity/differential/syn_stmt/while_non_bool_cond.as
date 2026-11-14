// @expect: reject
void f(int x) {
    while (x) {
        x--;
    }
}
