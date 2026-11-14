// @expect: accept
class Arr {
    int opIndex(int i) const { return i * 2; }
}
void main() {
    Arr a;
    int x = a[3];
}
