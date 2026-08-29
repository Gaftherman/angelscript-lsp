// PROP-07 refuted, and inverted: "Missing implementation of 'void I::P()'". An abstract class must
// still implement the interface it names. ClassRules skips this check for abstract classes, which
// is a missed diagnostic rather than a behaviour to keep.
interface I { void P(); }
abstract class A : I { A() {} }
class C2 : A { void P() {} }
