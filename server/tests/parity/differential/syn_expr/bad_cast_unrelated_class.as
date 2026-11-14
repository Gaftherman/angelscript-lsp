// @expect: accept
class Alpha { int a; }
class Beta { int b; }
void main() {
    Alpha a;
    Beta@ b = cast<Beta>(a);
}
