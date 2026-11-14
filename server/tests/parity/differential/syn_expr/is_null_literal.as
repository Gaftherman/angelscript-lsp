// @expect: accept
class Node {}
void main() {
    Node@ n = null;
    if (n is null) {}
    if (null !is n) {}
}
