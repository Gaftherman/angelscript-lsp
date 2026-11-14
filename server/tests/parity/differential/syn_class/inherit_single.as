// @expect: accept
class Base {
    int x;
}
class Derived : Base {
    int y;
}
void main() {
    Derived d;
    d.x = 1;
    d.y = 2;
}
