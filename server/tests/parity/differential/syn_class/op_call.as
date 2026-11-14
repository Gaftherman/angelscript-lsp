// @expect: accept
class Fn {
    int opCall(int x) const { return x + 1; }
}
void main() {
    Fn f;
    int r = f(10);
}
