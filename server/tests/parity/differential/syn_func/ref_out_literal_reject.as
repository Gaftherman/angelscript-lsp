// @expect: reject
void f(int &out x) { x = 1; }
void main() { f(42); }
