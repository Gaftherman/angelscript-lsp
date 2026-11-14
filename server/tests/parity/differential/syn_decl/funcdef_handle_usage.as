// @expect: accept
funcdef int BinaryOp(int a, int b);
void f() {
    BinaryOp@ op = null;
}
