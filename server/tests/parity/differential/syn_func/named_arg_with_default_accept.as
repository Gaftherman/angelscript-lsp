// @expect: accept
int f(int a = 1, int b = 2, int c = 3) { return a + b + c; }
void main() { int x = f(c: 10); }
