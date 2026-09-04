// KNOWN GAP, and reported as a miss by the audit rather than hidden.
//
//     angelscript_oracle doc_r13_return_base_as_derived.as
//         ERROR (12, 12): Can't implicitly convert from 'Base@&' to 'Derived@'.
//
// A base handle where a derived one is declared. The conversion rules here do not yet reason about
// which direction a hierarchy conversion runs, and the accepting direction - returning a derived
// handle from a base-handle function - is legal and common, so a rule that got the direction wrong
// would report ordinary code. Left silent until it can be got right.
class Base {}
class Derived : Base {}

Derived@ test(Base@ b)
{
    return b;
}
