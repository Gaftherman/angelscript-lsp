// TYPE-03, the float-truncation half. Accepted by the compiler - it is a warning - so this is a
// doc_p file and the parity audit judges it on errors alone. Measured with:
//
//     angelscript_oracle doc_p26_float_truncation.as
//
// The rule is one-directional and follows the value, not the declaration: a float reaching an
// integer slot warns wherever it is written, and the reverse never does.
//
// The silent half is the one that matters. A CONSTANT source is folded and judged by value, not
// by type, so it is either clean or answered by a different warning entirely:
//
//     const float D = 15.0; int m = D;   ->  clean, 15.0 survives exactly
//     const float D = 15.5; int m = D;   ->  "Implicit conversion of value is not exact"
//     float D = 15.0;       int m = D;   ->  "Float value truncated", which is this rule
//
// Getting that wrong is not hypothetical: the first run of the numeric corpus audit reported 34
// findings of the `const float WEAPON_DAMAGE = 15.0; int m_iBulletDamage = WEAPON_DAMAGE;`
// shape across the Sven Co-op weapon scripts, and the compiler is silent on every one.
//
// KNOWN GAP, one line: `int fromExpression = f * 2.0f;` below. The compiler warns; this analyzer
// does not, because the initializer path resolves through ResolveValueType, which has no
// binary-expression case and answers "cannot see enough to judge" - and that answer returns
// before any rule runs. The assignment path uses ResolveExpressionType and does catch it, which
// is why `assigned += f;` two lines up is reported. Closing it means teaching ResolveValueType
// about binary expressions, which every conversion ERROR in this file would then also flow
// through; that is a bigger change than one warning is worth, and a missed warning is the safe
// side of it. Everything else here matches the compiler line and column exactly.

float ReadF() { return 1.5f; }
double ReadD() { return 1.5; }
int ReadI() { return 1; }

const float K_EXACT = 15.0;
const float K_INEXACT = 15.5;

class Holder { int stored; }

void Warned()
{
    float f = ReadF();
    double d = ReadD();
    Holder h;

    int initialised = f;
    int64 wide = f;
    uint8 narrow = f;
    int fromDouble = d;

    int assigned = 0;
    assigned = f;
    assigned += f;

    h.stored = f;

    int fromExpression = f * 2.0f;
}

int ReturnsTruncated()
{
    float f = ReadF();
    return f;
}

void Silent()
{
    float f = ReadF();
    int i = ReadI();

    // Widening never warns, in either float direction.
    float widened = i;
    double promoted = f;
    float demoted = ReadD();

    // A written conversion is the author saying so.
    int converted = int(f);

    // A constant source is folded and judged by value. None of these is this rule's warning.
    int fromExactConst = K_EXACT;
    int fromInexactConst = K_INEXACT;
    int fromExactLiteral = 2.0f;
    int fromInexactLiteral = 1.5f;
}
