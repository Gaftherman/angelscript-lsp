// TYPE-05 in its true, narrow form. A typedef names a primitive, and the name is that type inside
// a template argument as much as anywhere else: `array<byte>` and `array<uint8>` are one
// instantiation, and the compiler accepts a call and an assignment between them in either
// direction.
//
// UnwrapTypedef in OverloadResolver ran on the outer name only, so the two were unrelated
// spellings and the call was reported - a false positive on legal code.
typedef uint8 byte;

void TakeBase(array<uint8> data)
{
}

void TakeAlias(array<byte> data)
{
}

void main()
{
    array<byte> alias;
    array<uint8> base;

    TakeBase(alias);
    TakeAlias(base);

    array<uint8> copy = alias;
    array<byte> back = base;

    array<array<byte>> nested;
    array<array<uint8>> nestedBase = nested;
}
