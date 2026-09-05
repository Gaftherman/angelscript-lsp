// An if statement cannot have an empty statement body.
//
//     angelscript_oracle doc_r30_if_empty_statement.as
//         ERROR (10, 15): If with empty statement
//
// A solitary semicolon after the if condition is rejected by the compiler.
void test()
{
    int x = 1;
    if (x > 0);
}
