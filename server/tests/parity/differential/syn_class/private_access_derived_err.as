// @expect: reject
class Base {
    private int code;
}
class Derived : Base {
    int read() const { return code; }
}
