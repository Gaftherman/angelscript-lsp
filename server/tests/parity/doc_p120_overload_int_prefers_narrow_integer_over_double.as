// An int argument against a narrower integer and a wider floating point type.
//
//     angelscript_oracle doc_p120_overload_int_prefers_narrow_integer_over_double.as
//         (accepted, no output)
//
// The compiler picks int16 - staying an integer beats staying lossless. Ranking a widening
// conversion into floating point above a narrowing one within the integers had it backwards.
class P120Narrow {}
class P120Wide {}

P120Narrow@ P120Pick(int16 a) { return null; }
P120Wide@ P120Pick(double a) { return null; }

void main()
{
    int p120Count = 3;
    P120Narrow@ chosen = P120Pick(p120Count);
}
