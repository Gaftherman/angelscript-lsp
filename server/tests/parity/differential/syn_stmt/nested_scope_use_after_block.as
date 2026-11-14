// @expect: reject
int f() {
    {
        int x = 1;
    }
    return x;
}
