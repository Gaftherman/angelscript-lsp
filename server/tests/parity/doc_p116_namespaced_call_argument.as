// A namespace-qualified call used as an argument, followed by more positional arguments.
//
//     angelscript_oracle doc_p116_namespaced_call_argument.as
//         (accepted, no output)
//
// The '::' is not a named argument. A named argument is `name: value`, and the grammar writes the
// ':' as a token of the argument list itself; the one inside a scope operator belongs to the
// expression. Reading the argument's text for a ':' instead reported 156 errors on one real Sven
// Co-op plugin file, on lines like this one.
namespace P116Names
{
    string GetName() { return "knuckles"; }
}

void P116Take(const string &in label, const string &in name, int price) {}

void main()
{
    P116Take("Brass Knuckles", P116Names::GetName(), 40);
}
