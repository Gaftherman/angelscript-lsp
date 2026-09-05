// A string cannot be used as a for-loop condition.
//
//     angelscript_oracle doc_r34_string_as_for_condition.as
//         ERROR (10, 12): Expression must be of boolean type, instead found 'string'
//
// For-loop conditions must evaluate to a boolean expression.
void test()
{
    string s = "test";
    for (; s; ) {}
}
