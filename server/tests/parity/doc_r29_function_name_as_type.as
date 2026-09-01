// A function handle needs a funcdef naming its signature. The compiler answers
// "Identifier 'Foo' is not a data type" - a function name is never a type on its own.
// The analyzer offers as-hint-funcdef-missing (opt-in) plus a quick fix that declares one.
void Foo(int a) {}
void main() { Foo@ h = @Foo; }
