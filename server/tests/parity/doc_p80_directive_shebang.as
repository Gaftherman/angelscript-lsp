#!/usr/bin/as
// A shebang line on the first line of a script.
//
//     angelscript_oracle doc_p80_directive_shebang.as
//         accepted, exit 0. A shebang is skipped by CScriptBuilder and is not a directive.
//
// CScriptBuilder skips a shebang line on the first line of the file, so it never reaches the
// compiler and compilation succeeds.
void DirShebangMain()
{
}
