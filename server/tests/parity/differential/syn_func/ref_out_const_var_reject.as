// @expect: reject
void f(int &out x) { x = 1; }
void main() { const int a = 1; f(a); }
