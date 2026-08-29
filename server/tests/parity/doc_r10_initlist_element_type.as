// TYPE-13, the part not yet checked: "Can't implicitly convert from 'const string' to 'int&'".
// InitializerListChecker validates the list's shape against the list pattern but never the type of
// an element.
void main() { array<int> a = { "x", "y" }; }
