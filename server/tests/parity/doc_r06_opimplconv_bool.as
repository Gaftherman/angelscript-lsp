// TYPE-07 refuted: "No conversion from 'H&' to 'bool' available." A bool opImplConv does not make
// a class usable as a condition.
class H { bool opImplConv() const { return true; } }
void main() { H h; if (h && true) {} }
