// `null` is not a value an int function can return.
//
//     angelscript_oracle doc_r10_return_null_primitive.as
//         ERROR (3, 5): No conversion from '<null handle>' to 'int' available.
//
// as-err-null-non-handle said exactly this about a variable and had nothing to say about a return.
// The rule is restricted to the primitives by name: a value type the host registered may accept
// null through a conversion this analyzer never sees.
int test()
{
    return null;
}
