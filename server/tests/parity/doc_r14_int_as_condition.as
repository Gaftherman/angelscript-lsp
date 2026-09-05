// A number is not a condition.
//
//     angelscript_oracle doc_r14_int_as_condition.as
//         ERROR (5, 9): Expression must be of boolean type, instead found 'int'
//
// AngelScript has no numeric-to-bool conversion, so this is rejected under every setting -
// asEP_BOOL_CONVERSION_MODE decides whether a *class* may convert and does not reach a primitive.
// `if`, `while`, `for`'s condition and `do ... while` were all measured, and so was `bool b = x;`,
// which answers "Can't implicitly convert from 'int' to 'bool'."
void test()
{
    int x = 1;
    if (x)
    {
    }
}
