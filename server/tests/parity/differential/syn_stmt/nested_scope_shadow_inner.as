// @expect: accept
int f() {
    int x = 1;
    {
        int x = 2;
        if (x > 1) return x;
    }
    return x;
}
