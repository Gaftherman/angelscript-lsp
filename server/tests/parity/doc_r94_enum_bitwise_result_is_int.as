// The shape a user reaches for when they think of an enum as a flag set. Two errors in one line,
// and the compiler names the argument first.
//
// angelscript_oracle:
//   ERROR (16, 9): No matching signatures to 'ParityEnumBitsIntTake(int)'
//   INFO  (16, 9): Candidates are:
//   INFO  (16, 9): void ParityEnumBitsIntTake(ParityEnumBitsIntProbe t)
//   INFO  (16, 9): Rejected due to type mismatch on parameter 't'
//
// `t & ~Member` is an int, and there is no implicit int -> enum. The assignment of a void call into
// an enum variable is wrong too; the cast that makes the argument legal is pinned in doc_p127.

enum ParityEnumBitsIntProbe
{
    ParityBitsIntA = 1,
    ParityBitsIntB = 2
}

void ParityEnumBitsIntTake(ParityEnumBitsIntProbe t)
{
}

void ParityEnumBitsIntUse()
{
    ParityEnumBitsIntProbe t = ParityEnumBitsIntProbe::ParityBitsIntA;

    t = ParityEnumBitsIntTake(t & ~ParityEnumBitsIntProbe::ParityBitsIntB);
}
