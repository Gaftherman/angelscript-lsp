// @expect: accept
class Num {
    int v;
    Num(int x = 0) { v = x; }
    Num opAdd(const Num &in rhs) const {
        return Num(v + rhs.v);
    }
}
void main() {
    Num a(1), b(2);
    Num c = a + b;
}
