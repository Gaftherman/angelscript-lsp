// Multiple named arguments passed out-of-order, mixed with optional parameters and container initializer lists.
//
//     angelscript_oracle doc_p128_named_arguments_multi_reordered.as
//         (accepted, no output)
void P128Funct(int id = 0, bool f = true, array<string> argS = array<string>()) {}

void main()
{
    P128Funct(argS: {"hi", "hellol"}, id: 1, f: false);
    P128Funct(argS: {"only"});
    P128Funct(10, argS: {"mixed"});
    P128Funct(f: false, id: 42);
}
