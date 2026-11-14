// @expect: accept
// @complete+ topAlpha
void topAlpha() {}
namespace N {
    void topAlpha() {}
    void f() {
        ::top/*$complete*/();
    }
}
