// @expect: reject
class Base {
    int f() final { return 1; }
}
class Derived : Base {
    int f() override { return 2; }
}
