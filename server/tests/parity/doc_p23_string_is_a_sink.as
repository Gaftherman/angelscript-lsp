// Every primitive reaches `string`, and an enum does too. The standard string add-on registers an
// `opAssign` for each of them, so all of the assignments below compile - measured one type at a
// time against the oracle, not read off the spec.
//
// `IsConvertible` had `string` in its built-in set and then answered `false` for every
// non-numeric pair, so `string s = i;` was reported. Between the direct assignments and the
// `"" + x` concatenations that produce the same question, this was **149 of the 273 findings** the
// corpus audit reported - all of it legal code.
//
// The direction matters: this is a sink, the mirror of what an enum is. Nothing comes back out of
// `string` implicitly, which is doc_r25.
enum Flag
{
    FlagOne = 1,
    FlagTwo = 2
}

void main()
{
    int8 i8 = 1;
    uint8 u8 = 1;
    int16 i16 = 1;
    uint16 u16 = 1;
    int i32 = 1;
    uint u32 = 1;
    int64 i64 = 1;
    uint64 u64 = 1;
    float f = 1.0f;
    double d = 1.0;
    bool b = true;
    Flag flag = FlagOne;

    string s8 = i8;
    string su8 = u8;
    string s16 = i16;
    string su16 = u16;
    string s32 = i32;
    string su32 = u32;
    string s64 = i64;
    string su64 = u64;
    string sf = f;
    string sd = d;
    string sb = b;
    string sflag = flag;

    string concatenated = "" + i32;
}
