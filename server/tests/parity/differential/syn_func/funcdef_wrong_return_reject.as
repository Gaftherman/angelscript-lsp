// @expect: reject
funcdef int Op(int);
void bad(int a) {}
void main() { Op@ fn = @bad; }
