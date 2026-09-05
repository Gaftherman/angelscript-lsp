// An enum body needs no semicolon after its closing brace.
//
//     angelscript_oracle doc_p35_enum_shift_no_semicolon.as   accepted
//
// Both spellings are legal - with the semicolon is doc_p36 - and so is a shift expression as the
// value, which is the idiom for a flag enum.
enum eSomeEnum
{
    Enum_1 = (1 << 2),
    Enum_2 = (1 << 3)
}

void test()
{
    int flags = Enum_1 | Enum_2;
    flags = flags & Enum_1;
}
