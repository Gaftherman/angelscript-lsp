// @expect: reject
class Base {
    Base(int a) {}
}
class Derived : Base {
    Derived() { super(1); }
    void f() { super(2); }
}
