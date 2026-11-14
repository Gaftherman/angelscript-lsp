// @expect: accept
void f(int a) {}
void f(float a) {}
void main() { f(1); f(1.5f); }
