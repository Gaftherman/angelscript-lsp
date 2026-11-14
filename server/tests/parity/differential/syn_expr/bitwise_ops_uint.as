// @expect: accept
void main() {
    uint a = 0xF0;
    uint b = 0x0F;
    uint c = (a & b) | (a ^ b);
    uint d = ~a;
    uint e = a << 2;
    uint f = a >> 1;
    uint g = a >>> 1;
}
