// PROP-01 refuted as stated: "'V' is not a member of 'C'". get_/set_ are ordinary methods until
// the `property` keyword is present (asEP_PROPERTY_ACCESSOR_MODE 3, the SDK default). Hosts that
// set mode 2 do get the document's behaviour, which is why this is a setting and not a rule.
//
// Both halves are now measurable rather than one, because the oracle takes the property:
//
//     angelscript_oracle doc_r07_accessor_without_kw.as --property-accessor-mode=3
//         ERROR (5, 21): 'V' is not a member of 'C'
//     angelscript_oracle doc_r07_accessor_without_kw.as --property-accessor-mode=2
//         accepted
//
// The parity audit runs without the flag, so this file is judged under the engine's own default of
// 3 and belongs in the `doc_r` set. The mode-2 half is asserted in AccessCheckerTest instead, since
// a `doc_p` file that only compiles under a non-default engine property would fail the audit for a
// reason that has nothing to do with the analyzer.
//
// This server defaults to mode 2, the more permissive of the two: under 2 it misses a diagnostic a
// mode-3 host would give, and under 3 it would invent one for a mode-2 host. Missing beats
// inventing. See EngineProperties::propertyAccessorMode and doc_p04 for the accepting form.
class C { private int m; int get_V() const { return m; } void set_V(int v) { m = v; } }
void main() { C c; c.V = 3; }
