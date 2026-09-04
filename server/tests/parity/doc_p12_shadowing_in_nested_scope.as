// The accepting half of doc_r08. A nested block opens a scope of its own, and so does each `for`,
// so the same name in each is not a redeclaration - measured, all three of these compile:
//
//     angelscript_oracle doc_p12_shadowing_in_nested_scope.as   accepted
//
// The rule that reports doc_r08 has to leave every one of these alone, which is the whole reason
// this file exists beside it: a duplicate-declaration rule that cannot tell a nested scope from a
// repeated one would report legal code, and that is the one thing this project does not do.
void F()
{
    float f = 1;
    {
        float f = 2;
        f += 1;
    }
    f += 1;

    for (int i = 0; i < 2; i++) { }
    for (int i = 0; i < 2; i++) { }

    if (f > 0) { int a = 1; a += 1; }
    else { int a = 2; a += 1; }
}
