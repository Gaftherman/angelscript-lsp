// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 9, 'Handle to handle is not allowed')]
// @expect: reject
class Obj {}
void f() {
    Obj@@ p = null;
}
