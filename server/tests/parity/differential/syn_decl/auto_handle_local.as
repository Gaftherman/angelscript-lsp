// @expect: accept
class Node {}
void f() {
    Node@ n = Node();
    auto@ m = n;
}
