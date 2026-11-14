// @expect: accept
class Obj {}
void f() {
    Obj a;
    Obj@ p = @a;
}
