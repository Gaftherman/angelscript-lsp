// A cast between two classes with no relation of any kind.
//
//     angelscript_oracle doc_p122_cast_between_unrelated_classes.as
//         (accepted, no output)
//
// `cast<>` is a dynamic cast. It answers null at runtime when the object is not of that type, which
// is why the language spells it this way rather than as a conversion - so "the types are unrelated"
// is never a compile-time verdict between two reference types.
//
// The companion of doc_p121, which covers the interface directions. Together they are the four
// shapes measured: class to class, class to interface, interface to class, interface to interface.
class P122Left
{
    int leftOnly;
}

class P122Right
{
    int rightOnly;
}

P122Left@ P122Get() { return null; }

void main()
{
    P122Right@ crossed = cast<P122Right@>(P122Get());

    // And from a value, not only from a call.
    P122Left held;
    P122Right@ alsoCrossed = cast<P122Right@>(held);
}
