// @expect: accept
int f(int a, int b = 2, int c = 3) { return a + b + c; }
void main() { int x = f(1, c: 10); }
