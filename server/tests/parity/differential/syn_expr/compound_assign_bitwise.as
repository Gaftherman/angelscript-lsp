// @expect: accept
void main() {
    uint x = 15;
    x &= 7;
    x |= 8;
    x ^= 3;
    x <<= 2;
    x >>= 1;
    x >>>= 1;
}
