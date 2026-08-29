// PROP-01 refuted as stated: "'V' is not a member of 'C'". get_/set_ are ordinary methods until
// the `property` keyword is present (asEP_PROPERTY_ACCESSOR_MODE 3, the SDK default). Hosts that
// set mode 2 do get the document's behaviour, which is why this is a setting and not a rule.
class C { private int m; int get_V() const { return m; } void set_V(int v) { m = v; } }
void main() { C c; c.V = 3; }
