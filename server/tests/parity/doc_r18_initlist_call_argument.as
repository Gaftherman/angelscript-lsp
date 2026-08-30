// An initializer list as a call argument. The compiler infers the target type from the parameter
// and compiles the list against it:
//
//   ERROR (14, 11): Can't implicitly convert from 'const string' to 'int&'.
//
// InitializerListChecker only ever visited a variable_declaration, so an argument list was checked
// nowhere - not its shape, not its contents.
void Take(array<int> values)
{
}

void main()
{
    Take({"x"});
}
