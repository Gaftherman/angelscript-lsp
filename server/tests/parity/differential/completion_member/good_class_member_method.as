// @expect: accept
// @complete+ BrookFlow
// @complete+ BrookRipples
class BrookWater {
    int BrookDepth;
    void BrookFlow() {}
    void BrookRipples() {}
}
void run(BrookWater& b) {
    b.Br/*$complete*/();
}
