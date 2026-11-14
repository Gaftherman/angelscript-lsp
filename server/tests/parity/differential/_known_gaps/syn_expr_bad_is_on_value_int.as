// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(5, 16, 'Both operands must be handles when comparing identity')]
// @expect: reject
void main() {
    int a = 1;
    int b = 1;
    bool r = a is b;
}
