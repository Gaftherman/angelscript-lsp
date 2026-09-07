// A positional argument after a named one.
//
//     angelscript_oracle doc_r86_positional_after_named_arg.as
//         INFO (11, 1): Compiling void main()
//         ERROR (13, 12): Positional arguments cannot be passed after named arguments
//
// The case as-err-positional-after-named-arg exists for, kept beside doc_p116 and doc_p117 so that
// narrowing the rule to the ':' token of the argument list cannot quietly switch it off.
void R86Take(int a, int b, int c) {}

void main()
{
    R86Take(a: 1, 2, 3);
}
