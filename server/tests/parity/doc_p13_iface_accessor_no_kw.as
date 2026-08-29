// PROP-02. An interface may declare get_/set_ methods. They are only reachable as the property `P`
// where the `property` keyword is allowed, which is not on an interface - see doc_r02.
interface IE { int get_Health() const; }
class E : IE { int get_Health() const { return 1; } }
void main() { IE@ e = E(); int h = e.get_Health(); print("" + h); }
