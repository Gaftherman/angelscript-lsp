// A well-formed #if / #endif block with hashes touching the names.
//
//     angelscript_oracle doc_p81_directive_if_endif.as
//         accepted, exit 0.
//
// Both hashes touch their directive names. When the identifier is undefined, CScriptBuilder
// blanks out the entire block before the compiler runs.
void DirIfEndifMain()
{
}

#if SOMEUNDEFINEDWORD
int dirIfEndifStray = 1;
#endif
