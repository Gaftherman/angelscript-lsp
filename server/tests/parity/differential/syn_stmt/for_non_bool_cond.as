// @expect: reject
void f() {
    for (int i = 0; i; i++) {
    }
}
