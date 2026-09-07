// A conditional expression used as an argument, followed by another positional argument.
//
//     angelscript_oracle doc_p117_ternary_argument.as
//         (accepted, no output)
//
// The other half of doc_p116: the ':' of `a ? b : c` is inside the conditional expression, not a
// token of the argument list, so this is two positional arguments and no named one.
void P117Take(int first, int second) {}

void main()
{
    bool p117Flag = true;
    P117Take(p117Flag ? 1 : 2, 5);
}
