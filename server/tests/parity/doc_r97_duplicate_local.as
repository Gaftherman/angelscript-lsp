// A local name declared twice in one scope. Found by hand, in a stub the user was writing, where
// the only thing the analyzer said about it was that neither of the two was ever used.
//
//     angelscript_oracle doc_r08_duplicate_local.as
//         ERROR (14, 11): 'f' is already declared
//
// A function's parameters count as part of its body for this rule - `void F(float f) { float f; }`
// is the same error - while a nested block is a scope of its own and shadowing inside one is
// accepted. Both measured; see doc_p12 for the accepting form.
void SomeFunction()
{
    float f;
    f = 1;
    float f;
    f = 2;
}
