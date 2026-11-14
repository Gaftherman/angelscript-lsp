// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 7, "No conversion from 'bool' to math type available.")]
// @expect: reject
void main() {
    bool b = true;
    b += false;
}
