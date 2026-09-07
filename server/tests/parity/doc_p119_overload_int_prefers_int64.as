// An int argument against float, int64 and uint64 overloads, and nothing else - the shape the Sven
// Co-op stub declares Math.min with.
//
//     angelscript_oracle doc_p119_overload_int_prefers_int64.as
//         (accepted, no output)
//
// int -> int64 keeps the signedness, int -> uint64 does not, and int -> float leaves the kind, so
// the three are three ranks and not one. Scoring the first two the same made a real script's
// `Math.max( 1, Math.min( 255, numBubbles ) )` an ambiguous call.
class P119Signed {}
class P119Unsigned {}
class P119Float {}

P119Float@ P119Pick(float a) { return null; }
P119Signed@ P119Pick(int64 a) { return null; }
P119Unsigned@ P119Pick(uint64 a) { return null; }

void main()
{
    int p119Count = 3;

    // The handle type is what makes the choice observable: this only compiles if the int64
    // overload is the one picked.
    P119Signed@ chosen = P119Pick(p119Count);
}
