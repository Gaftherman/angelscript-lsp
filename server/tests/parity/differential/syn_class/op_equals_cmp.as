// @expect: accept
class Num {
    int v;
    Num(int x = 0) { v = x; }
    bool opEquals(const Num &in rhs) const { return v == rhs.v; }
    int opCmp(const Num &in rhs) const { return v - rhs.v; }
}
void main() {
    Num a(1), b(2);
    bool eq = (a == b);
    bool lt = (a < b);
}
