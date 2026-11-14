// @expect: reject
void f(int &inout x) { x += 1; }
void main() { int a = 1; f(a); }
