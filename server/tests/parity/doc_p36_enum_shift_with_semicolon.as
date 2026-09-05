// The same enum with a semicolon after the closing brace, which is also legal.
//
//     angelscript_oracle doc_p36_enum_shift_with_semicolon.as   accepted
//
// Paired with doc_p35 on purpose: the semicolon is optional, so neither spelling may be reported.
enum eSomeEnum
{
    Enum_1 = (1 << 2),
    Enum_2 = (1 << 3)
};

void test()
{
    int flags = Enum_1 | Enum_2;
    flags = flags & Enum_2;
}
