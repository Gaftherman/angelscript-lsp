// @expect: reject
void f(int n) { if (n > 0) { int x = f(n - 1); } }
void main() {}
