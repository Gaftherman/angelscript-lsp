// Indented conditional directives.
//
//     angelscript_oracle doc_p82_directive_indented.as
//         accepted, exit 0.
//
// Whitespace BEFORE the hash is fine; only whitespace AFTER it is not. That pair is the whole rule.
void DirIndentedMain()
{
}

    #if SOMEUNDEFINEDWORD
    int dirIndentedStray = 1;
    #endif
