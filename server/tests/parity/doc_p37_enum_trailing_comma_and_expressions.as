// A trailing comma after the last value, and values that are expressions.
//
//     angelscript_oracle doc_p37_enum_trailing_comma_and_expressions.as   accepted
//
// A negative value, a value computed from an earlier one, and a bitwise-or in parentheses are all
// accepted. The trailing comma after `C` is the part most likely to be reported by mistake.
enum EExpressions
{
    A = -1,
    B = A + 2,
    C = (1 | 4),
}

void test()
{
    int v = A + B + C;
    v = v * 2;
}
