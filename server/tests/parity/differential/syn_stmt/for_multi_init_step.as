// @expect: accept
int f() {
    int sum = 0;
    for (int i = 0, j = 10; i < j; i++, j--) {
        sum += i + j;
    }
    return sum;
}
