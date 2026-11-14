// @expect: reject
class Item { int v; }
void f() {
    const Item@ p = Item();
    p.v = 1;
}
