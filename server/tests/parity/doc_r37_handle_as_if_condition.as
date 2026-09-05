// A handle is not a null test.
//
//     angelscript_oracle doc_r37_handle_as_if_condition.as
//         ERROR (13, 9): Expression must be of boolean type, instead found 'HandleTarget@&'
//
// The mistake carried in from C++, where a pointer in a condition tests for null. AngelScript wants
// the comparison spelled out - `if (h !is null)`, measured accepted, and kept as doc_p38.
class HandleTarget { int v; }

void test()
{
    HandleTarget@ h = HandleTarget();
    if (h)
    {
        h.v = 1;
    }
}
