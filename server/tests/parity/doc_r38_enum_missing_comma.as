// Enum values are separated by commas, and the comma is not optional.
//
//     angelscript_oracle doc_r38_enum_missing_comma.as
//         ERROR (8, 21): Unexpected token '<identifier>'
//
// A trailing comma after the LAST value is fine - see doc_p35 - but a missing one between two
// values is a parse error, not a recoverable style choice.
enum EComma { A = 1 B = 2 }

void test()
{
    int v = A;
    v = v + 1;
}
