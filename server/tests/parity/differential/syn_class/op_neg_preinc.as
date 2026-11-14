// @expect: accept
class Counter {
    int v;
    Counter(int x = 0) { v = x; }
    Counter opNeg() const { return Counter(-v); }
    Counter &opPreInc() { ++v; return this; }
}
void main() {
    Counter c(5);
    Counter n = -c;
    ++c;
}
