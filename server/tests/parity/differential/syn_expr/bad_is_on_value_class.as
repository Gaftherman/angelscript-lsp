// @expect: accept
class Box { int v; }
void main() {
    Box a;
    Box b;
    bool r = a !is b;
}
