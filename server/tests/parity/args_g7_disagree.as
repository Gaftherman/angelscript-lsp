class A {}
class B {}
void g(int a, A b) {}
void g(A a, A b) {}
void main() { B x; g(1, x); }
