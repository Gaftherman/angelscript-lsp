// @expect: accept
mixin class MyMixin {
    int m;
    int getM() const { return m; }
}
class Host : MyMixin {
    void test() { m = 5; }
}
