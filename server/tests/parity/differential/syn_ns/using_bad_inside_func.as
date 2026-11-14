// @expect: accept
namespace A { void foo() {} }
void main() {
    using namespace A;
    foo();
}
