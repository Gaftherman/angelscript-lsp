// @expect: accept
// @complete+ CloudKind
namespace Sky {
    enum CloudKind {
        Stratus,
        Cumulus
    }
}
class CloudStorm {}
void observe(Sky::Cl/*$complete*/ kind) {}
