// @expect: accept
class Base {
    protected int code;
}
class Derived : Base {
    int read() const { return code; }
}
