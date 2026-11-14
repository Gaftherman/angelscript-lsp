// @expect: reject
funcdef int Op(int, int);
int inc(int a) { return a + 1; }
void main() { Op@ fn = @inc; }
