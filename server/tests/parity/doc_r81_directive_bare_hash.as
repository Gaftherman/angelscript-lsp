// A line containing only a bare hash is not a directive.
//
//     angelscript_oracle doc_r81_directive_bare_hash.as
//         ERROR (8, 1): Unexpected token '<unrecognized token>'
//
// CScriptBuilder leaves anything it does not recognise in the source, and the compiler rejects
// the bare '#' as an unexpected token.
#

void DirBareHashMain()
{
}
