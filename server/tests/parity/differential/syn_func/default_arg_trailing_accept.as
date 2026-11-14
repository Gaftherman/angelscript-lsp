// @expect: accept
int add(int a, int b = 10) { return a + b; }
void main() { int x = add(5) + add(5, 2); }
