// A space between the hash and the directive name.
//
//     angelscript_oracle doc_r82_directive_space_after_hash.as
//         ERROR (8, 1): Unexpected token '<unrecognized token>'
//
// CScriptBuilder requires the directive name to touch the '#' directly. With whitespace between
// them, it is not recognised as a directive and is rejected by the compiler.
# include "helper.as"

void DirSpaceAfterHashMain()
{
}
