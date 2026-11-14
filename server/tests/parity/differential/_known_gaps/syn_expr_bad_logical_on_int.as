// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(5, 16, "No conversion from 'int' to 'bool' available.")]
// @expect: reject
void main() {
    int a = 1;
    int b = 2;
    bool c = a && b;
}
