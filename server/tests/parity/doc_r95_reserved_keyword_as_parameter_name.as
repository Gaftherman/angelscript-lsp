// A reserved keyword cannot be a parameter name.
//
// angelscript_oracle:
//   ERROR (1, 29): Expected ')' or ','
//   ERROR (1, 29): Instead found reserved keyword 'true'
//
// It is a parse error rather than a semantic one, so what this fixture guards is that the parser
// does not quietly accept it - and that whatever it reports lands on the parameter list.

void ParityReservedParamProbe(bool true, bool false)
{
}
