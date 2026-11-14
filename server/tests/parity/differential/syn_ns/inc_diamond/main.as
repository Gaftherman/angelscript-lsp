// @expect: accept
#include "left.as"
#include "right.as"
int total() { return leftFn() + rightFn(); }
