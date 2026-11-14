// @expect: reject
int sub(int a, int b) { return a - b; }
void main() { int x = sub(a: 1, 2); }
