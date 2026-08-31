// The boundary of doc_p29. These two declarations sit in a hierarchy and are NOT an override pair:
// their parameter lists differ - `int` against `int &out` - so neither replaces the other, and an
// `int` argument matches both. The compiler rejects it:
//
//     angelscript_oracle doc_r28_hierarchy_ambiguity.as
//         ERROR: Multiple matching signatures to 'Derived::Get(int)'
//
// This is what keeps the override rule from swallowing real findings. The rule compares parameter
// TYPES and MODIFIERS, so `int` and `int &out` stay two candidates, exactly as they are here.

class Base
{
    bool Get(int value) { return value > 0; }
}

class Derived : Base
{
    bool Get(int &out value) { value = 1; return true; }
}

void main()
{
    Derived d;
    int n = 0;
    d.Get(n);
}
