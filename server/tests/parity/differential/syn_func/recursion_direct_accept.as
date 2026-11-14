// @expect: accept
int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
void main() { int x = fact(5); }
