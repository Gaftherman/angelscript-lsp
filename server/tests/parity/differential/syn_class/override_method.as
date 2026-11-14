// @expect: accept
class Base {
    int f() { return 1; }
}
class Derived : Base {
    int f() override { return 2; }
}
