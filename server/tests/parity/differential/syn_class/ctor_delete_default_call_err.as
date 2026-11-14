// @expect: reject
class NoDefault {
    NoDefault() = delete;
    NoDefault(int x) {}
}
void main() {
    NoDefault a;
}
