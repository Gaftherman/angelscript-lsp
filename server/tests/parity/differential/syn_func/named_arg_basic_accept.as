// @expect: accept
int sub(int a, int b) { return a - b; }
void main() { int x = sub(b: 2, a: 5); }
