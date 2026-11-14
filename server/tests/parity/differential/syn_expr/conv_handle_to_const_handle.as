// @expect: accept
class Item { int id; }
void main() {
    Item@ a = Item();
    const Item@ b = a;
}
