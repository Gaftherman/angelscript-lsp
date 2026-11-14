// @expect: accept
int f() {
    {
        int x = 1;
    }
    {
        int x = 2;
        return x;
    }
}
