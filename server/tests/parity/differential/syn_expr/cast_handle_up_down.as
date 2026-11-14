// @expect: accept
class Base { int v; }
class Derived : Base { int w; }
void main() {
    Derived d;
    Base@ b = cast<Base>(d);
    Derived@ d2 = cast<Derived>(b);
}
