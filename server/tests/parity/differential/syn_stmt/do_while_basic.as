// @expect: accept
int f(int n) {
    int i = 0;
    do {
        i++;
    } while (i < n);
    return i;
}
