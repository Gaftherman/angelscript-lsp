// TYPE-03, the signed/unsigned half. The compiler ACCEPTS every line below - the mismatch is a
// warning, not an error - so this belongs in the doc_p set, and the parity audit judges it on
// errors alone. What it records is the shape of the warning, which the backlog had wrong on
// three counts. Measured with:
//
//     angelscript_oracle doc_p25_signed_unsigned_mismatch.as
//
//   * It fires ONLY on the six comparison operators. `i * u`, `i & u`, assignment, argument
//     passing and return are all silent. The backlog implied it followed the conversion.
//   * Width is irrelevant, and float and double count as SIGNED: `float < uint` warns while
//     `float < int` does not.
//   * A compile-time constant on either side folds the comparison away and it is silent.
//
// That last one is the whole false-positive surface, and it is why the analyzer stays silent
// whenever either operand is constant - which costs `u < -5`, a case the compiler does warn
// about. Missing beats inventing. See TypeConversionChecker.cpp's numeric-warning section.

int ReadI() { return 1; }
uint ReadU() { return 1; }
float ReadF() { return 1.0f; }

const int G_CONST = 1;

class Counter { uint total; }

void Warned()
{
    int i = ReadI();
    uint u = ReadU();
    float f = ReadF();
    Counter c;

    // Six comparisons, six warnings, each anchored on the operator.
    bool a = i < u;
    bool b = i > u;
    bool d = i <= u;
    bool e = i >= u;
    bool g = i == u;
    bool h = i != u;

    // Float and double are the signed side of the pairing.
    bool k = f < u;

    // Neither a member read, a call, nor an arithmetic subexpression folds.
    bool m = i < c.total;
    bool n = i < ReadU();
    bool o = i < u + u;
}

void Silent()
{
    int i = ReadI();
    uint u = ReadU();
    float f = ReadF();

    // Arithmetic and bitwise pairings say nothing at all.
    uint sum = i + u;
    uint prod = i * u;
    uint masked = i & u;

    // Nor does assignment, argument passing or return.
    int narrowed = u;
    uint widened = i;
    TakesInt(u);
    TakesUint(i);

    // Same signedness, any widths.
    int64 big = 1;
    bool p = i < big;

    // Float against a SIGNED integer is not a mismatch.
    bool q = f < i;

    // A constant on either side is folded, so there is nothing left to mismatch.
    const int localConst = 1;
    const uint localUnsigned = 2;
    bool r = localConst < u;
    bool s = i < localUnsigned;
    bool t = G_CONST < u;
    bool v = i < 5;
    bool w = u < 5;
}

void TakesInt(int) {}
void TakesUint(uint) {}
