// @expect: reject
class Num {
    int opAdd(int a, int b) const { return a + b; }
}
void main() {
    Num a, b;
    int c = a + b;
}
