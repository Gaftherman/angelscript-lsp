// @expect: reject
int fact(int n) { if (n <= 1) return 1; return n * fact("bad"); }
void main() {}
