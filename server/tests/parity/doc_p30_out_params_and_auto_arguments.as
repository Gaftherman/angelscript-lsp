// Two things an overload set is allowed to do that this analyzer used to refuse. Every call below
// is ACCEPTED:
//
//     angelscript_oracle doc_p30_out_params_and_auto_arguments.as
//
// 1. An `&out` parameter takes any NUMERIC type, converting on the way back out. It does not need
//    an exact match. Measured one type at a time against a single `int &out`:
//
//        uint accepted   float accepted   int64 accepted
//        string  "No matching signatures to 'S::Get(string)'"
//
//    `bool` is refused in both directions, as it is everywhere else - see doc_r27. So the test is
//    "both numeric", not "identical", and it costs four corpus findings that read
//    `schema.Get("minItems", uiTemp)` with a `uint` as "No matching signatures".
//
// 2. An argument whose type this analyzer cannot pin down must not decide between overloads. An
//    unresolved argument scores an exact match against every parameter - the right answer for "do
//    not reject" - and that made every candidate tie, so the call was reported "Multiple matching
//    signatures". `auto` is the spelling that reached a verdict; an empty type is stopped earlier.
//    The compiler resolves the `auto` and picks the one overload that fits, which is exactly what
//    this analyzer cannot do and must therefore not pretend to.

class Reader
{
    bool Get(const string &in key, int &out value) { value = 1; return true; }
    bool Get(const string &in key, float &out value) { value = 1.0f; return true; }
    bool Get(int &out value) { value = 1; return true; }
}

class Item {}
class Version {}

void Start(Item@ item) {}
void Start(const Version &in version) {}

void NumericOutParameters()
{
    Reader reader;

    // Every numeric type into an `int &out` / `float &out` set.
    uint asUint;
    int asInt;
    float asFloat;
    int64 asWide;
    double asDouble;

    reader.Get("minItems", asUint);
    reader.Get("minItems", asInt);
    reader.Get("minItems", asFloat);
    reader.Get(asWide);
    reader.Get(asDouble);
}

void AutoArguments()
{
    array<Item@> items;
    items.insertLast(Item());

    // `auto` from a subscript, then handed to an overload set. The compiler knows what it is;
    // this analyzer does not, and stays quiet rather than calling the call ambiguous.
    auto fromSubscript = items[0];
    Start(fromSubscript);

    Item@ handle = items[0];
    auto@ fromHandle = handle;
    Start(fromHandle);
}
