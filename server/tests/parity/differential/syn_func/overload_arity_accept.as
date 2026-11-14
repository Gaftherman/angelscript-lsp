// @expect: accept
int f(int a) { return a; }
int f(int a, int b) { return a + b; }
void main() { int x = f(1) + f(1, 2); }
