// @expect: reject
void f(int &inout x) { x = 1; }
void main() { f(42); }
