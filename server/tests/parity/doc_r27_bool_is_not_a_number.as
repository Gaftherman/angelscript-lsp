// `bool` is not a number and converts to none of them, in either direction. Every line below is
// REJECTED by the compiler. Measured with:
//
//     angelscript_oracle doc_r27_bool_is_not_a_number.as
//
// The full matrix was measured before this file was written: {bool -> T, T -> bool} x {argument,
// initializer} over all ten numeric types, forty combinations, forty rejections. This file keeps
// one of each shape rather than all forty, and adds the forms the matrix does not cover - the
// explicit casts, arithmetic, a return, and a condition.
//
// This is a rejection the analyzer used to MISS, and missing it was not the expensive half.
// `bool` was listed as convertible in OverloadResolver's widening and narrowing tables and in
// IsConvertible, so an int argument scored as viable against a `bool` parameter - and against a
// `bool&out` one. Two overloads differing only in `Get(bool&out, bool)` versus
// `Get(int&out, bool)` therefore tied on an int argument and the call was reported "Multiple
// matching signatures", which the compiler accepts without hesitating. 75 times over the corpus,
// in a JSON library it carries twice. See doc_p28 for the calls that must stay silent.

void TakesInt(int v) {}
void TakesFloat(float v) {}
void TakesBool(bool v) {}

int ReturnsInt()
{
    bool b = true;
    return b;                   // No conversion from 'bool' to 'int' available.
}

bool ReturnsBool()
{
    int n = 1;
    return n;                   // No conversion from 'int' to 'bool' available.
}

void Initializers()
{
    bool b = true;
    int n = 1;
    float f = 1.0f;

    int fromBool = b;           // Can't implicitly convert from 'bool' to 'int'.
    double alsoFromBool = b;    // ... and the same for every other numeric type.
    bool fromInt = n;           // Can't implicitly convert from 'int' to 'bool'.
    bool fromFloat = f;
}

void Arguments()
{
    bool b = true;
    int n = 1;
    float f = 1.0f;

    TakesInt(b);                // No matching signatures to 'TakesInt(bool)'
    TakesFloat(b);              // No matching signatures to 'TakesFloat(bool)'
    TakesBool(n);               // No matching signatures to 'TakesBool(int)'
    TakesBool(f);               // No matching signatures to 'TakesBool(float)'
}

void WrittenOutInFull()
{
    bool b = true;
    int n = 1;

    // Writing the conversion does not make it exist: there is no such conversion to name.
    int explicitFromBool = int(b);
    bool explicitFromInt = bool(n);

    // Nor is bool a math type.
    int arithmetic = b + 1;
}

void AsACondition()
{
    int n = 1;
    if (n) { }                  // Expression must be of boolean type, instead found 'int'
}
