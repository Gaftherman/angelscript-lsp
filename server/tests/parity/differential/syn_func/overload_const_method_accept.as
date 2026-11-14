// @expect: accept
class C { int get() { return 1; } int get() const { return 2; } }
void main() { C c; const C cc; int a = c.get() + cc.get(); }
