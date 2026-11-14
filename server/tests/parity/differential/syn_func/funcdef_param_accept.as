// @expect: accept
funcdef int Op(int);
int apply(Op@ fn, int v) { return fn(v); }
int dbl(int x) { return x * 2; }
void main() { int y = apply(@dbl, 3); }
