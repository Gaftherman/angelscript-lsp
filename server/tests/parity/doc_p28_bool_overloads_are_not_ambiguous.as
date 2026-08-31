// The accepting half of doc_r27, and the half that cost something. Every line here is ACCEPTED:
//
//     angelscript_oracle doc_p28_bool_overloads_are_not_ambiguous.as
//
// An overload set differing only in `bool` versus a numeric type is not ambiguous, because only
// one of those parameters can take the argument at all. The analyzer used to disagree: `bool` was
// listed as convertible to and from the numeric types in OverloadResolver's widening and
// narrowing tables, so both candidates scored viable, tied, and the call was reported "Multiple
// matching signatures". 75 findings over the corpus, all of them legal code, concentrated in a
// JSON library whose reader is written exactly like the class below.
//
// The `&out` forms are the ones the corpus actually writes, and they are the strictest case: an
// out-reference takes its type exactly, so nothing about `int` makes `bool&out` a candidate.

class Reader
{
    bool Get(bool &out value, bool strict = true) const { value = true; return strict; }
    bool Get(int &out value, bool strict = true) const { value = 1; return strict; }
    bool Get(float &out value, bool strict = true) const { value = 1.0f; return strict; }
    bool Get(string &out value) const { value = ""; return true; }
}

void ByOutReference()
{
    Reader reader;
    bool strict = true;

    bool asBool;
    int asInt;
    float asFloat;
    string asString;

    reader.Get(asBool, strict);
    reader.Get(asInt, strict);
    reader.Get(asFloat, strict);
    reader.Get(asString);
}

void ByValue(bool b, int n, float f)
{
    // The same set without the references. Still one candidate each, still not ambiguous.
    TakesOne(b);
    TakesOne(n);
    TakesOne(f);
}

void TakesOne(bool v) {}
void TakesOne(int v) {}
void TakesOne(float v) {}

void BoolStaysItself()
{
    bool b = true;

    // bool to bool, and bool into the string sink the add-on really does register.
    bool copy = b;
    string text = b;
    TakesOne(b);

    // A condition is a bool, which is the one thing bool is for.
    if (b) { }
    while (b) { break; }
}
