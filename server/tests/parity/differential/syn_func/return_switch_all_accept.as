// @expect: accept
int f(int x) { switch (x) { case 1: return 10; default: return 20; } }
void main() { int y = f(1); }
