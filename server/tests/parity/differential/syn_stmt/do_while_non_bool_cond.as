// @expect: reject
void f(int x) {
    do {
        x--;
    } while (x);
}
