// @expect: accept
namespace A {
    namespace B {
        int getVal() { return 1; }
    }
}
int x = A::B::getVal();
