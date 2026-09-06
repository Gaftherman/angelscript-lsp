// Spaces after the hash in conditional directives.
//
//     angelscript_oracle doc_r83_directive_space_before_if.as
//         ERROR (12, 1): Unexpected token '<unrecognized token>'
//
// This is the case that mattered most, because reading `#  if` as a directive would exclude the
// block below it and silence every diagnostic inside a region the compiler actually keeps.
void DirSpaceBeforeIfMain()
{
}

#  if SOMEWORD
#  endif
