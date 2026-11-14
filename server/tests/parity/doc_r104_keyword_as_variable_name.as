// A keyword cannot be a variable name.
//
//     angelscript_oracle doc_r15_keyword_as_variable_name.as
//         ERROR (12, 9): Expected '('
//
// The message reads like a complaint about punctuation and is not: the parser takes `int` as a type
// and goes looking for the conversion `int(...)`. What it is rejecting is a name that can never be
// a name. The same check already guarded a class name and a typedef name and had never been asked
// about a variable, so this analyzer said only that the variable was never used.
void test()
{
    int int;
}
