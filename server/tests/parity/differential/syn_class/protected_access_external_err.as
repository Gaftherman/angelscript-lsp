// @expect: reject
class Base {
    protected int code;
}
void main() {
    Base b;
    b.code = 1;
}
