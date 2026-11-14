// @expect: accept
class Foo {}
void main() {
    Foo@ f = null;
    if (f !is null) {}
    if (f is null) {}
}
