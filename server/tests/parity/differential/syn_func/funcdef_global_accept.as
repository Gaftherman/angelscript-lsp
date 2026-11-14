// @expect: accept
funcdef int Op(int, int);
int add(int a, int b) { return a + b; }
void main() { Op@ fn = @add; int y = fn(1, 2); }
