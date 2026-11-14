// A bare `return;` in a function that owes a value.
//
//     angelscript_oracle doc_r09_return_without_value.as
//         ERROR (12, 5): Must return a value
//
// as-err-not-all-paths-return did not see this: it asks whether a return is *reached*, and one is.
// The opposite direction - a value returned from a void function - is as-err-void-return-value and
// has been reported for much longer, which is how a function could be missing half of the rule
// without anything noticing.
float FS(float f)
{
    return;
}
