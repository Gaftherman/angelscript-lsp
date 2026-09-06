// Directive names are case-sensitive.
//
//     angelscript_oracle doc_r84_directive_wrong_case.as
//         ERROR (8, 1): Unexpected token '<unrecognized token>'
//
// Directive names are case-sensitive. CScriptBuilder does not recognise `#Include`, leaving it in
// the source where the compiler rejects it.
#Include "helper.as"

void DirWrongCaseMain()
{
}
