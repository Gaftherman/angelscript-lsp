// @expect: reject
int sub(int a, int b) { return a - b; }
void main() { int x = sub(z: 1, a: 2); }
