// @expect: accept
class Vec {
    int x;
    Vec() { x = 0; }
    Vec(int a) { x = a; }
}
void main() {
    Vec a;
    Vec b(5);
}
