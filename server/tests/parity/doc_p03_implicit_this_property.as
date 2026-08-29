// PROP-04. A bare `Up` inside a method resolves to this.get_Up() - but only because the accessor
// carries the `property` keyword. Drop the keyword and the compiler answers
// "No matching symbol 'Up'": see doc_r07.
class C
{
    int get_Up() const property { return 1; }
    void T() { int v = Up; print("" + v); }
}
