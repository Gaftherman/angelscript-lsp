// @expect: reject
int add(int a, int b = 1) { return a + b; }
void main() { int x = add(1, 2, 3); }
