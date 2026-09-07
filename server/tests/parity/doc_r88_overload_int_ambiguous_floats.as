// An int argument against both floating point types.
//
//     angelscript_oracle doc_r88_overload_int_ambiguous_floats.as
//         INFO  (11, 1): Compiling void main()
//         ERROR (14, 5): Multiple matching signatures to 'R88Pick(int)'
//         INFO  (14, 5): void R88Pick(float a)
//         INFO  (14, 5): void R88Pick(double a)
//
// float and double tie with each other, the same way two signedness changes do in doc_r87. The
// other half of the evidence that put floating point last as one rank rather than two.
void R88Pick(float a) {}
void R88Pick(double a) {}

void main()
{
    int r88Count = 3;
    R88Pick(r88Count);
}
