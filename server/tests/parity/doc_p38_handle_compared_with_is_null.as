// The correct spelling of the test doc_r37 gets wrong.
//
//     angelscript_oracle doc_p38_handle_compared_with_is_null.as   accepted
//
// `!is null` is how a handle is tested. Kept next to doc_r37 so the pair says what to write, not
// only what not to.
class NullTarget { int v; }

void test()
{
    NullTarget@ h = NullTarget();
    if (h !is null)
    {
        h.v = 1;
    }
}
