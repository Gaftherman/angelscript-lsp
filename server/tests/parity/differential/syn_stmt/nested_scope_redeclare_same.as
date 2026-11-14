// @expect: reject
void f() {
    {
        int x = 1;
        int x = 2;
    }
}
