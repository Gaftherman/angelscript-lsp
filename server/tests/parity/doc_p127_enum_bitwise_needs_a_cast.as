// A bitwise expression over enumerators has type `int`, not the enum, so it needs an explicit
// conversion before it can go back where the enum is expected.
//
// angelscript_oracle: exit 0, no diagnostics.
//
// The rejected form is pinned in doc_r94: passing the bare `int` result to a parameter typed as the
// enum is `No matching signatures`.

enum ParityEnumBitsProbe
{
    ParityBitsA = 1,
    ParityBitsB = 2
}

void ParityEnumBitsTake(ParityEnumBitsProbe t)
{
}

void ParityEnumBitsUse()
{
    ParityEnumBitsProbe t = ParityEnumBitsProbe::ParityBitsA;

    ParityEnumBitsTake(ParityEnumBitsProbe(t & ~ParityEnumBitsProbe::ParityBitsB));
}
