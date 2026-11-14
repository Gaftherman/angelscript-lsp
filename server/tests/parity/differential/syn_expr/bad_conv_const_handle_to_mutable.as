// @expect: reject
class Item { int id; }
void main() {
    const Item@ a = Item();
    Item@ b = a;
}
