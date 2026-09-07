// An int argument against two unsigned overloads of very different widths.
//
//     angelscript_oracle doc_r87_overload_int_ambiguous_signedness.as
//         INFO  (11, 1): Compiling void main()
//         ERROR (14, 5): Multiple matching signatures to 'R87Pick(int)'
//         INFO  (14, 5): void R87Pick(uint8 a)
//         INFO  (14, 5): void R87Pick(uint64 a)
//
// Both change the signedness, and width does not break that tie however far apart the two are.
// Kept beside doc_p119 so that giving a signedness change its own rank cannot turn the rule off.
void R87Pick(uint8 a) {}
void R87Pick(uint64 a) {}

void main()
{
    int r87Count = 3;
    R87Pick(r87Count);
}
