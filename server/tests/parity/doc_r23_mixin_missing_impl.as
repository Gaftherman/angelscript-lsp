// A mixin that names an interface must implement it *itself*. This is the half of the rule that
// needed the compiler to settle, because the intuition runs the other way: a mixin has no
// instances, so leaving the interface to whoever includes it looks reasonable, and that is what
// `rules/ClassRules.cpp` used to do - it skipped the check for a mixin and for an abstract class
// alike. Both were missed diagnostics.
//
// The compiler's answer to this file:
//
//     ERROR: Missing implementation of 'void IThink::Think()'
//
// reported against Helper, even though Agent below implements Think perfectly well. Implementing
// it in the including class does not satisfy the mixin. The accepting form is doc_p21.
interface IThink
{
    void Think();
}

mixin class Helper : IThink
{
    void Assist() {}
}

class Agent : Helper
{
    void Think() {}
}
