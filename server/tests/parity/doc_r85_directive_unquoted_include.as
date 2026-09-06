// An include directive without quotes.
//
//     angelscript_oracle doc_r85_directive_unquoted_include.as
//         ERROR (8, 1): Unexpected token '<unrecognized token>'
//
// CScriptBuilder requires quotes around the include filename. An unquoted path is not processed as
// a valid include directive and is rejected by the compiler.
#include helper.as

void DirUnquotedIncludeMain()
{
}
