// @expect: accept
class Node { int v; }
void main() {
    Node@ a = Node();
    Node@ b = a;
    bool same = a is b;
    bool diff = a !is null;
}
