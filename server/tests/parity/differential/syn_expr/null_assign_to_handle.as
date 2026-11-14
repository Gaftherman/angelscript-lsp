// @expect: accept
class Foo {}
void main() {
    Foo@ f = Foo();
    @f = null;
}
