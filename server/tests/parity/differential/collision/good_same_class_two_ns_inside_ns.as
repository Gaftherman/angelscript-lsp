// @expect: accept
// @complete+ CliffPeak
namespace North {
    class CliffPeak {}
}
namespace South {
    class CliffPeak {}
    void climb() {
        Cl/*$complete*/ c;
    }
}
