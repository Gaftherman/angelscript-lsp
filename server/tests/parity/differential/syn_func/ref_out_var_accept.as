// @expect: accept
void f(int &out x) { x = 5; }
void main() { int a; f(a); }
