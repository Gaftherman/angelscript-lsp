// A reference cast whose target is a primitive.
//
//     angelscript_oracle doc_r89_cast_to_primitive.as
//         INFO  (15, 1): Compiling void main()
//         ERROR (18, 25): Illegal target type for reference cast
//
// The half of as-err-invalid-cast that survives. Between two reference types the cast is always
// legal - doc_p121 and doc_p122 - so a primitive on either side is the only thing left for the rule
// to claim, and this keeps narrowing it from quietly becoming switching it off.
class R89Holder
{
    int value;
}

void main()
{
    R89Holder@ held;
    int narrowed = cast<int>(held);
}
