// @expect: accept
namespace A { namespace B { class Item { int id; } } }
void useItem() { A::B::Item it; it.id = 3; }
