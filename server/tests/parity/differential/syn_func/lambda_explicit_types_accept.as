// @expect: accept
funcdef int BinOp(int, int);
void main() { BinOp@ op = function(int a, int b) { return a * b; }; int y = op(3, 4); }
