// An integer assigned into an enum that lives in a namespace.
//
//     angelscript_oracle doc_r91_int_into_namespaced_enum.as
//         INFO  (24, 1): Compiling void main()
//         ERROR (26, 31): Can't implicitly convert from 'int' to 'R91Flags::Kind'.
//     asharness  doc_r91_int_into_namespaced_enum.as
//         Can't implicitly convert from 'int' to 'R91Flags::Kind'.
//
// The inward direction, which the compiler closes: an enum widens out to an integer and nothing
// widens in. doc_p125 is the outward half.
//
// This one started being reported by the same change that fixed doc_p125 - typing an enum member
// with the enum's qualified name made the enum findable, and a rule that had been silent for want
// of it started answering. Kept because a fix that turns a false positive into a false negative has
// not fixed anything, and this is the check that says which happened.
namespace R91Flags
{
    enum Kind
    {
        None = 0,
        AdminOnly = 1
    }
}

void main()
{
    R91Flags::Kind narrowed = 1;
}
