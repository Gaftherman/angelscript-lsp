// An else branch cannot have an empty statement body.
//
//     angelscript_oracle doc_r31_else_empty_statement.as
//         ERROR (10, 23): Else with empty statement
//
// A solitary semicolon after the else keyword is rejected by the compiler.
void test()
{
    int x = 1;
    if (x > 0) {} else;
}
