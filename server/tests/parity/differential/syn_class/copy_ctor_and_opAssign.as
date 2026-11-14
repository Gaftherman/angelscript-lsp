// @expect: accept
class Item {
    int v;
    Item() { v = 0; }
    Item(const Item &in other) { v = other.v; }
    Item &opAssign(const Item &in other) {
        v = other.v;
        return this;
    }
}
void main() {
    Item a;
    a.v = 9;
    Item b(a);
    Item c;
    c = b;
}
