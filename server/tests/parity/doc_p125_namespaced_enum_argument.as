// An enum declared inside a namespace, passed where an integer is expected.
//
//     angelscript_oracle doc_p125_namespaced_enum_argument.as
//         (accepted, no output)
//     asharness  doc_p125_namespaced_enum_argument.as
//         (accepted, no output)
//
// An enum is an integer with a name, so it widens out to one - but only when the analyzer can find
// the enum, and it could not. The symbol table keys `P125Flags::Kind` under its qualified name
// while the collector typed each member with the simple one, so every lookup of `Kind` came back
// empty and the argument was reported as a conversion that does not exist.
//
// Found by running the 1061-script corpus against a real Sven Co-op stub: 43
// as-err-no-matching-constructor and part of 236 as-err-no-implicit-conversion, including
// MapChooser's `CClientCommand(..., ConCommandFlag::AdminOnly)`.
//
// The same value reaching the call through a variable declared `P125Flags::Kind` always worked,
// which is what said the bug was in typing the member expression rather than in the rule. Both
// spellings are here so the difference cannot come back unnoticed. doc_r91 holds the direction the
// compiler really does reject.
namespace P125Flags
{
    enum Kind
    {
        None = 0,
        AdminOnly = 1
    }
}

typedef uint P125Flags_t;

void P125TakeUint(uint flags) {}
void P125TakeTypedef(const P125Flags_t flags) {}

void main()
{
    // Through the member, qualified by its namespace.
    P125TakeUint(P125Flags::AdminOnly);
    P125TakeTypedef(P125Flags::AdminOnly);

    // Through the enum's own scope, which is a third spelling of the same value.
    P125TakeUint(P125Flags::Kind::AdminOnly);

    // And through a variable, which is the spelling that always worked.
    P125Flags::Kind held = P125Flags::AdminOnly;
    P125TakeUint(held);
}
