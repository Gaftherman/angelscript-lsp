// @expect: reject
class Base {}
class Derived : Base {
    int f() override { return 2; }
}
