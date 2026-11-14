// @expect: accept
namespace A::B::C {
    void ping() {}
}
void main() { A::B::C::ping(); }
