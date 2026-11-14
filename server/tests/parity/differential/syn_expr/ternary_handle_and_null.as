// @expect: accept
class Obj {}
void main() {
    bool cond = true;
    Obj@ o = cond ? Obj() : null;
}
