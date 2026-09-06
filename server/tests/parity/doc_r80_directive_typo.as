// A misspelled preprocessor directive is not recognised by CScriptBuilder.
//
//     angelscript_oracle doc_r80_directive_typo.as
//         ERROR (8, 1): Unexpected token '<unrecognized token>'
//
// CScriptBuilder leaves anything it does not recognise in the source, and the compiler then
// rejects it.
#incude "something"

void DirTypoMain()
{
}
