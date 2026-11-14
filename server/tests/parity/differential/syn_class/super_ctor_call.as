// @expect: accept
class Base {
    int x;
    Base(int a) { x = a; }
}
class Derived : Base {
    Derived(int a) {
        super(a);
    }
}
