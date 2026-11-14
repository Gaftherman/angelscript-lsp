// @expect: accept
int f(bool c) { if (c) return 1; else return 2; }
void main() { int x = f(true); }
