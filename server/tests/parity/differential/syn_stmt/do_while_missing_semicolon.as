// @expect: reject
void f(int n) {
    int i = 0;
    do {
        i++;
    } while (i < n)
}
