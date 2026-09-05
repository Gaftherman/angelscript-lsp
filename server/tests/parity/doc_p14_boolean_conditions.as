// The accepting side of doc_r14, and the reason that rule is restricted to the primitives by name.
//
//     angelscript_oracle doc_p14_boolean_conditions.as   accepted
//
// A bool, a comparison, a handle tested with `!is`, and a logical operator over them. A rule that
// could not tell these from `if (x)` on an int would report ordinary code, which is the one thing
// this project does not do.
class Thing {}

void test()
{
    bool ready = true;
    int count = 3;
    Thing@ handle = null;

    if (ready) { }
    if (count != 0) { }
    if (handle !is null) { }
    if (ready && count > 1) { }
    while (count > 0) { count--; }
    for (int i = 0; i < count; i++) { }
}
