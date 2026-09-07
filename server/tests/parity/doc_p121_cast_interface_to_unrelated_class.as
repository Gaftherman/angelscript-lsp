// A cast from an interface handle to a class that does not declare that interface.
//
//     angelscript_oracle doc_p121_cast_interface_to_unrelated_class.as
//         (accepted, no output)
//
// An interface handle says nothing about the class behind it. The object may be an instance of a
// class this file has never seen, so "the types are unrelated" is not a verdict available at
// compile time - `cast<>` answers null at runtime when the guess is wrong, which is what it is for.
//
// Found against a real Sven Co-op plugin: `cast<CIns2GL@>(CastToScriptClass(pEntity))` is how the
// game hands a script its own object back, and the engine's return type is an interface. Seven
// errors on code that runs.
interface P121Iface {}

class P121Implements : P121Iface
{
    int declared;
}

class P121Unrelated
{
    int other;
}

P121Iface@ P121Get() { return null; }

void main()
{
    // The related direction, which was already accepted.
    P121Implements@ related = cast<P121Implements@>(P121Get());

    // And the one that was reported: nothing in this file connects the two.
    P121Unrelated@ unrelated = cast<P121Unrelated@>(P121Get());
}
