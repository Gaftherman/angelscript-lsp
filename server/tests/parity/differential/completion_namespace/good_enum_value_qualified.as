// @expect: accept
// @complete+ CirrusCloud
// @complete+ CumulusCloud
namespace Sky {
    enum CloudKind {
        CirrusCloud,
        CumulusCloud
    }
}
void test() {
    int k = Sky::C/*$complete*/;
}
