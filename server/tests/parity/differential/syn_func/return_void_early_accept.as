// @expect: accept
void f(bool c) { if (c) return; int x = 1; }
void main() { f(false); }
