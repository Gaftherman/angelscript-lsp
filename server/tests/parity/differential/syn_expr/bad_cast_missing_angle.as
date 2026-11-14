// @expect: reject
class Base {}
void main() {
    Base b;
    Base@ b2 = cast(Base)(b);
}
