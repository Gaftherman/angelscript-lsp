// A string cannot be used directly as an if condition.
//
//     angelscript_oracle doc_r33_string_as_if_condition.as
//         ERROR (10, 9): Expression must be of boolean type, instead found 'string'
//
// AngelScript has no implicit conversion from string to boolean.
void test()
{
    string s = "test";
    if (s) {}
}
