// TYPE-11. With a primitive on the left the compiler looks for opMul_r on the right operand.
class V { V opMul_r(float s) const { return this; } }
void main() { V v; V r = 2.0f * v; }
