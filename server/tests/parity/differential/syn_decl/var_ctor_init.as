// @expect: accept
class Box {
    int x;
    Box(int v) { x = v; }
}
void f() {
    Box b(42);
}
