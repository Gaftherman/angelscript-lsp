// @expect: accept
funcdef void Cb();
class C { void run() {} }
void main() { C c; Cb@ cb = Cb(c.run); cb(); }
