// @expect: accept
void topFn() {}
namespace A {
    void topFn() {}
    void test() { ::topFn(); }
}
