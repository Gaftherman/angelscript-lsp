// Dangling else after an empty if body is rejected by the compiler.
//
//     angelscript_oracle doc_r32_dangling_else_empty_if.as
//         ERROR (9, 26): Expected expression value
//
// The semicolon ends the if statement, so the else belongs to no if.
bool test(bool c)
{
    if (c); return true; else return false;
}
